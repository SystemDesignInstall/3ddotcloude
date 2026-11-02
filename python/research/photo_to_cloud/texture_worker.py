#!/usr/bin/env python3
"""Create a textured GLB from a mesh and its matching COLMAP workspace."""

from __future__ import annotations

import argparse
import json
import mimetypes
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
from urllib.parse import unquote, urlsplit

from mesh_worker import inspect_mesh, publish_without_replacement


MAX_THREADS = 64
IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".tif", ".tiff"}
GLB_JSON_CHUNK = 0x4E4F534A
MAX_GLB_JSON_BYTES = 64 * 1024 * 1024
MAX_GLB_FILE_BYTES = 0xFFFFFFFF


class TextureError(RuntimeError):
    pass


class TextureCancelled(TextureError):
    pass


def _check_cancel(cancel_file: Path | None) -> None:
    if cancel_file is not None and cancel_file.exists():
        raise TextureCancelled("Создание текстурированной модели остановлено пользователем.")


def inspect_workspace(path: Path) -> dict:
    """Accept either dense_workspace itself or its reconstruction result folder."""
    try:
        workspace = path.expanduser().resolve(strict=True)
    except OSError as exc:
        raise TextureError(f"Папка реконструкции не найдена: {exc}") from exc
    if (workspace / "dense_workspace").is_dir():
        workspace = workspace / "dense_workspace"
    images = workspace / "images"
    if not workspace.is_dir() or not images.is_dir():
        raise TextureError(
            "Нужна папка dense_workspace с подпапкой images. Выберите dense_workspace "
            "или папку результата реконструкции, где она находится."
        )
    image_count = sum(1 for candidate in images.rglob("*")
                      if candidate.is_file() and candidate.suffix.casefold() in IMAGE_EXTENSIONS)
    if image_count < 3:
        raise TextureError("Для текстурирования нужны минимум три undistorted-фотографии в dense_workspace/images.")
    sparse = workspace / "sparse"
    model_roots = [sparse, *sorted((p for p in sparse.glob("*") if p.is_dir()), key=lambda p: p.name)] \
        if sparse.is_dir() else []
    has_model = any(
        all(any((root / f"{prefix}{suffix}").is_file() for suffix in (".bin", ".txt"))
            for prefix in ("cameras", "images"))
        for root in model_roots
    )
    if not has_model:
        raise TextureError("В dense_workspace/sparse не найдена модель камер COLMAP.")
    return {"workspace": workspace, "images": images, "image_count": image_count}


def _executable(directory: Path, name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    path = directory / (name + suffix)
    if not path.is_file() or (os.name != "nt" and not os.access(path, os.X_OK)):
        raise TextureError(f"Не найдена программа {path}. Установите OpenMVS 2.4.0.")
    return path.resolve()


def inspect_glb(path: Path) -> dict:
    """Validate a single-file GLB and require embedded texture content."""
    try:
        size = path.stat().st_size
        with path.open("rb") as stream:
            header = stream.read(12)
            if len(header) != 12:
                raise TextureError("OpenMVS создал пустой или неполный GLB.")
            magic, version, declared_size = struct.unpack("<4sII", header)
            if magic != b"glTF" or version != 2 or declared_size != size:
                raise TextureError("OpenMVS создал некорректный GLB 2.0.")
            json_document = None
            offset = 12
            while offset < declared_size:
                chunk_header = stream.read(8)
                if len(chunk_header) != 8:
                    raise TextureError("GLB обрезан внутри списка блоков.")
                chunk_size, chunk_type = struct.unpack("<II", chunk_header)
                offset += 8
                if chunk_size % 4 or offset + chunk_size > declared_size:
                    raise TextureError("GLB содержит некорректный размер блока.")
                if chunk_type == GLB_JSON_CHUNK:
                    if chunk_size > MAX_GLB_JSON_BYTES:
                        raise TextureError("Заголовок GLB превышает допустимый размер.")
                    json_document = json.loads(stream.read(chunk_size).decode("utf-8").rstrip(" \t\r\n\0"))
                else:
                    stream.seek(chunk_size, os.SEEK_CUR)
                offset += chunk_size
            if offset != declared_size or json_document is None:
                raise TextureError("В GLB отсутствует JSON-блок.")
    except TextureError:
        raise
    except (OSError, ValueError, UnicodeError, json.JSONDecodeError, struct.error) as exc:
        raise TextureError(f"Не удалось проверить текстурированную модель GLB: {exc}") from exc

    if not isinstance(json_document, dict):
        raise TextureError("GLB содержит некорректный JSON-документ.")
    images = json_document.get("images", [])
    textures = json_document.get("textures", [])
    materials = json_document.get("materials", [])
    buffers = json_document.get("buffers", [])
    buffer_views = json_document.get("bufferViews", [])
    if any(not isinstance(collection, list) for collection in
           (images, textures, materials, buffers, buffer_views)):
        raise TextureError("GLB содержит некорректные списки изображений, текстур или материалов.")
    if not images or not textures or not materials:
        raise TextureError("GLB создан, но в нём не найдены встроенные фототекстуры и материалы.")
    if any(not isinstance(item, dict) for item in [*images, *buffers]):
        raise TextureError("GLB содержит некорректные записи изображений или данных.")
    if any(item.get("uri") is not None and not isinstance(item.get("uri"), str)
           for item in [*images, *buffers]):
        raise TextureError("GLB содержит некорректный URI встроенного ресурса.")
    external_resources = [item.get("uri") for item in [*images, *buffers]
                          if item.get("uri") and not item["uri"].startswith("data:")]
    if external_resources:
        raise TextureError("GLB ссылается на внешние файлы и не является автономной моделью.")
    for image in images:
        uri = image.get("uri", "")
        if uri.startswith("data:"):
            continue
        view_index = image.get("bufferView")
        mime_type = image.get("mimeType", "")
        if (not isinstance(view_index, int) or isinstance(view_index, bool)
                or not 0 <= view_index < len(buffer_views)
                or not isinstance(mime_type, str) or not mime_type.startswith("image/")):
            raise TextureError("GLB содержит фототекстуру без корректных встроенных данных.")
        view = buffer_views[view_index]
        if not isinstance(view, dict):
            raise TextureError("GLB содержит некорректное описание встроенной текстуры.")
        try:
            buffer_index = int(view["buffer"])
            byte_offset = int(view.get("byteOffset", 0))
            byte_length = int(view["byteLength"])
            buffer_length = int(buffers[buffer_index]["byteLength"])
        except (KeyError, IndexError, TypeError, ValueError) as exc:
            raise TextureError("GLB содержит некорректное описание встроенной текстуры.") from exc
        if (buffer_index < 0 or byte_offset < 0 or byte_length <= 0
                or byte_offset + byte_length > buffer_length):
            raise TextureError("Встроенная фототекстура выходит за границы бинарных данных GLB.")
    return {"image_count": len(images), "texture_count": len(textures), "material_count": len(materials)}


def embed_external_glb_images(path: Path) -> Path:
    """Pack OpenMVS's sidecar texture images into GLB buffer views."""
    path = path.resolve(strict=True)
    file_size = path.stat().st_size
    try:
        with path.open("rb") as source:
            header = source.read(12)
            if len(header) != 12:
                raise TextureError("OpenMVS создал пустой или неполный GLB.")
            magic, version, declared_size = struct.unpack("<4sII", header)
            if magic != b"glTF" or version != 2 or declared_size != file_size:
                raise TextureError("OpenMVS создал некорректный GLB 2.0.")
            chunks: list[dict] = []
            document = None
            binary_chunk = None
            offset = 12
            while offset < declared_size:
                chunk_header = source.read(8)
                if len(chunk_header) != 8:
                    raise TextureError("GLB обрезан внутри списка блоков.")
                chunk_size, chunk_type = struct.unpack("<II", chunk_header)
                data_offset = offset + 8
                if chunk_size % 4 or data_offset + chunk_size > declared_size:
                    raise TextureError("GLB содержит некорректный размер блока.")
                if chunk_type == GLB_JSON_CHUNK:
                    if document is not None or chunk_size > MAX_GLB_JSON_BYTES:
                        raise TextureError("В GLB отсутствует единственный допустимый JSON-блок.")
                    try:
                        document = json.loads(source.read(chunk_size).decode("utf-8").rstrip(" \t\r\n\0"))
                    except (ValueError, UnicodeError) as exc:
                        raise TextureError(f"Не удалось прочитать структуру GLB: {exc}") from exc
                    chunks.append({"type": chunk_type, "offset": None, "size": chunk_size})
                else:
                    if chunk_type == 0x004E4942:
                        if binary_chunk is not None:
                            raise TextureError("GLB содержит несколько бинарных блоков.")
                        binary_chunk = {"offset": data_offset, "size": chunk_size}
                    chunks.append({"type": chunk_type, "offset": data_offset, "size": chunk_size})
                    source.seek(chunk_size, os.SEEK_CUR)
                offset = data_offset + chunk_size
            if offset != declared_size or not isinstance(document, dict):
                raise TextureError("В GLB отсутствует JSON-блок.")

        images = document.get("images", [])
        if not isinstance(images, list):
            raise TextureError("GLB содержит некорректный список изображений.")
        external_images = []
        parent = path.parent.resolve()
        for image in images:
            if not isinstance(image, dict):
                raise TextureError("GLB содержит некорректную запись изображения.")
            uri = image.get("uri")
            if not uri or uri.startswith("data:"):
                continue
            parsed_uri = urlsplit(uri)
            if parsed_uri.scheme or parsed_uri.netloc or parsed_uri.query or parsed_uri.fragment:
                raise TextureError("OpenMVS создал текстуру по внешнему URL вместо локального файла.")
            resource_name = unquote(parsed_uri.path).replace("\\", "/")
            if not resource_name or resource_name.startswith("/") or ".." in resource_name.split("/"):
                raise TextureError("GLB ссылается на файл текстуры за пределами временной папки.")
            resource = (parent / Path(resource_name)).resolve()
            try:
                resource.relative_to(parent)
            except ValueError as exc:
                raise TextureError("GLB ссылается на файл текстуры за пределами временной папки.") from exc
            if not resource.is_file():
                raise TextureError(f"Не найдена текстура OpenMVS: {resource.name}.")
            mime_type = image.get("mimeType") or mimetypes.guess_type(resource.name)[0]
            if not isinstance(mime_type, str) or not mime_type.startswith("image/"):
                raise TextureError(f"Не удалось определить формат текстуры {resource.name}.")
            if "bufferView" in image:
                raise TextureError("GLB изображение одновременно ссылается на URI и bufferView.")
            external_images.append((image, resource, mime_type))

        if not external_images:
            return path
        buffers = document.get("buffers", [])
        if (not isinstance(buffers, list) or len(buffers) != 1 or not isinstance(buffers[0], dict)
                or "uri" in buffers[0]
                or binary_chunk is None):
            raise TextureError("OpenMVS GLB имеет неподдерживаемую структуру бинарных данных.")
        buffer_views = document.setdefault("bufferViews", [])
        if not isinstance(buffer_views, list):
            raise TextureError("GLB содержит некорректные bufferView данные.")
        buffer_document = buffers[0]
        try:
            initial_byte_length = int(buffer_document["byteLength"])
        except (KeyError, TypeError, ValueError) as exc:
            raise TextureError("GLB содержит некорректную длину бинарного блока.") from exc
        if (initial_byte_length < 0 or initial_byte_length > binary_chunk["size"]
                or binary_chunk["size"] - initial_byte_length > 3):
            raise TextureError("Длина бинарного блока GLB не совпадает с заголовком.")

        append_offset = binary_chunk["size"]
        final_byte_length = append_offset
        image_records = []
        for image, resource, mime_type in external_images:
            image_size = resource.stat().st_size
            if image_size <= 0:
                raise TextureError(f"Файл текстуры {resource.name} пуст.")
            view_index = len(buffer_views)
            buffer_views.append({"buffer": 0, "byteOffset": append_offset, "byteLength": image_size})
            image.pop("uri", None)
            image["bufferView"] = view_index
            image["mimeType"] = mime_type
            image_records.append((resource, image_size))
            final_byte_length = append_offset + image_size
            append_offset = (final_byte_length + 3) & ~3

        buffer_document["byteLength"] = final_byte_length
        json_bytes = json.dumps(document, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        json_bytes += b" " * (-len(json_bytes) % 4)
        if len(json_bytes) > MAX_GLB_JSON_BYTES:
            raise TextureError("Заголовок GLB превышает допустимый размер после упаковки текстур.")
        binary_size = append_offset
        total_size = 12 + sum(8 + (len(json_bytes) if chunk["type"] == GLB_JSON_CHUNK else
                                      binary_size if chunk["type"] == 0x004E4942 else chunk["size"])
                               for chunk in chunks)
        if not any(chunk["type"] == 0x004E4942 for chunk in chunks):
            total_size += 8 + binary_size
        if total_size > MAX_GLB_FILE_BYTES:
            raise TextureError("Текстурированная модель превышает максимальный размер GLB 2.0.")

        packed_path = path.with_name(path.stem + "-embedded.glb")
        if packed_path.exists():
            raise TextureError("Временный файл упаковки GLB уже существует.")
        with path.open("rb") as source, packed_path.open("xb") as target:
            target.write(struct.pack("<4sII", b"glTF", 2, total_size))
            wrote_binary = False
            for chunk in chunks:
                if chunk["type"] == GLB_JSON_CHUNK:
                    target.write(struct.pack("<II", len(json_bytes), GLB_JSON_CHUNK))
                    target.write(json_bytes)
                elif chunk["type"] == 0x004E4942:
                    target.write(struct.pack("<II", binary_size, 0x004E4942))
                    source.seek(binary_chunk["offset"])
                    remaining = binary_chunk["size"]
                    while remaining:
                        data = source.read(min(1024 * 1024, remaining))
                        if not data:
                            raise TextureError("Бинарный блок GLB неожиданно обрезан.")
                        target.write(data)
                        remaining -= len(data)
                    for resource, resource_size in image_records:
                        with resource.open("rb") as texture_stream:
                            remaining = resource_size
                            while remaining:
                                data = texture_stream.read(min(1024 * 1024, remaining))
                                if not data:
                                    raise TextureError(f"Текстура {resource.name} неожиданно обрезана.")
                                target.write(data)
                                remaining -= len(data)
                        target.write(b"\0" * (-resource_size % 4))
                    wrote_binary = True
                else:
                    target.write(struct.pack("<II", chunk["size"], chunk["type"]))
                    source.seek(chunk["offset"])
                    remaining = chunk["size"]
                    while remaining:
                        data = source.read(min(1024 * 1024, remaining))
                        if not data:
                            raise TextureError("Дополнительный блок GLB неожиданно обрезан.")
                        target.write(data)
                        remaining -= len(data)
            if not wrote_binary:
                target.write(struct.pack("<II", binary_size, 0x004E4942))
                for resource, resource_size in image_records:
                    with resource.open("rb") as texture_stream:
                        while data := texture_stream.read(1024 * 1024):
                            target.write(data)
                    target.write(b"\0" * (-resource_size % 4))
            target.flush()
            os.fsync(target.fileno())
        return packed_path
    except TextureError:
        raise
    except (OSError, ValueError, UnicodeError, struct.error) as exc:
        raise TextureError(f"Не удалось встроить текстуры в GLB: {exc}") from exc


def _run_program(args: list[str], cwd: Path, cancel_file: Path | None = None) -> None:
    print("Запуск:", " ".join(args), flush=True)
    _check_cancel(cancel_file)
    try:
        process = subprocess.Popen(args, cwd=cwd, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except OSError as exc:
        raise TextureError(f"Не удалось запустить {args[0]}: {exc}") from exc
    while process.poll() is None:
        if cancel_file is not None and cancel_file.exists():
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            raise TextureCancelled("Создание текстурированной модели остановлено пользователем.")
        time.sleep(0.1)
    _check_cancel(cancel_file)
    if process.returncode != 0:
        raise TextureError(f"{Path(args[0]).name} завершился с кодом {process.returncode}.")


def build_textured_model(mesh_path: Path, workspace_path: Path, openmvs_dir: Path,
                         output_path: Path, threads: int, cancel_file: Path | None = None,
                         runner=_run_program) -> dict:
    """Rebuild a temporary OpenMVS scene and bake one standalone textured GLB."""
    try:
        mesh_path = mesh_path.expanduser().resolve(strict=True)
        openmvs_dir = openmvs_dir.expanduser().resolve(strict=True)
        output_path = output_path.expanduser().resolve()
    except OSError as exc:
        raise TextureError(f"Не удалось открыть выбранный путь: {exc}") from exc
    if not mesh_path.is_file() or mesh_path.suffix.casefold() != ".ply":
        raise TextureError("Выберите файл сетки PLY.")
    if output_path.suffix.casefold() != ".glb":
        raise TextureError("Для текстурированной модели выберите расширение .glb.")
    if mesh_path == output_path:
        raise TextureError("Файл результата должен быть отдельным от исходной сетки.")
    if not output_path.parent.is_dir():
        raise TextureError("Папка для GLB не существует.")
    if output_path.exists():
        raise TextureError("Файл GLB уже существует. Выберите другое имя; существующий файл не изменён.")
    if not 1 <= threads <= MAX_THREADS:
        raise TextureError(f"Число потоков должно быть от 1 до {MAX_THREADS}.")
    if not openmvs_dir.is_dir():
        raise TextureError("Выберите папку установки OpenMVS 2.4.0.")
    metadata = inspect_mesh(mesh_path)
    workspace = inspect_workspace(workspace_path)
    interface = _executable(openmvs_dir, "InterfaceCOLMAP")
    texturer = _executable(openmvs_dir, "TextureMesh")
    _check_cancel(cancel_file)

    started = time.time()
    with tempfile.TemporaryDirectory(prefix="spc-texture-", dir=output_path.parent) as temporary:
        work = Path(temporary)
        scene = work / "scene.mvs"
        output_base = work / "textured-model"
        print(f"Сетка: {mesh_path.name} ({metadata['vertex_count']:,} вершин, {metadata['face_count']:,} граней).", flush=True)
        print(f"Фото с позами камер: {workspace['image_count']} из dense_workspace; создаю сцену OpenMVS…", flush=True)
        runner([str(interface), "--input-file", str(workspace["workspace"]), "--output-file", str(scene),
                "--image-folder", str(workspace["images"]), "--max-threads", str(threads)], work, cancel_file)
        if not scene.is_file() or scene.stat().st_size == 0:
            raise TextureError("InterfaceCOLMAP не создал сцену камер OpenMVS.")
        _check_cancel(cancel_file)
        print("Накладываю фотографии на поверхность и сохраняю GLB…", flush=True)
        runner([str(texturer), "--input-file", str(scene), "--mesh-file", str(mesh_path),
                # OpenMVS strips everything after the last dot in --output-file.
                # A relative basename avoids dots in the user's parent path being
                # mistaken for an output-file extension on Windows and Linux.
                "--output-file", output_base.name, "--export-type", "glb",
                "--max-threads", str(threads), "--working-folder", str(work)], work, cancel_file)
        candidate = output_base.with_suffix(".glb")
        embedded_candidate = embed_external_glb_images(candidate)
        glb = inspect_glb(embedded_candidate)
        _check_cancel(cancel_file)
        try:
            publish_without_replacement(embedded_candidate, output_path)
        except FileExistsError as exc:
            raise TextureError("Файл GLB появился во время работы; существующий файл сохранён.") from exc
        except OSError as exc:
            raise TextureError(f"Не удалось сохранить GLB: {exc}") from exc

    report = {
        "status": "succeeded",
        "operation": "photo_texture_baking",
        "backend": "OpenMVS TextureMesh 2.4.0",
        "input_mesh": mesh_path.name,
        "output": output_path.name,
        "registered_images": workspace["image_count"],
        "mesh_vertices": metadata["vertex_count"],
        "mesh_faces": metadata["face_count"],
        "glb_images": glb["image_count"],
        "glb_textures": glb["texture_count"],
        "materials": glb["material_count"],
        "threads": threads,
        "compute_backend": "OpenMVS build default; CPU thread limit requested",
        "elapsed_seconds": round(time.time() - started, 3),
        "limitations": [
            "Mesh geometry must remain in the same camera coordinate system as dense_workspace.",
            "Faces not visible in the source images may remain untextured or use a fallback color.",
            "The GLB is textured but is not georeferenced and inherits the reconstruction scale.",
        ],
    }
    print(f"Готово: {output_path.name}; {glb['texture_count']} текстур, {metadata['face_count']:,} граней.", flush=True)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="Наложение фотографий на 3D-сетку в формат GLB")
    parser.add_argument("--mesh", required=True, type=Path)
    parser.add_argument("--workspace", required=True, type=Path)
    parser.add_argument("--openmvs-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--cancel-file", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        build_textured_model(args.mesh, args.workspace, args.openmvs_dir, args.output,
                             args.threads, args.cancel_file)
        return 0
    except TextureCancelled as exc:
        print(f"Остановлено: {exc}", file=sys.stderr, flush=True)
        return 130
    except Exception as exc:
        print(f"Ошибка текстурирования сетки: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
