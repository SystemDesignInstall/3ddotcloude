#!/usr/bin/env python3
"""Create a textured GLB from a mesh and its matching COLMAP workspace."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

from mesh_worker import inspect_mesh, publish_without_replacement


MAX_THREADS = 64
IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".tif", ".tiff"}
GLB_JSON_CHUNK = 0x4E4F534A
MAX_GLB_JSON_BYTES = 64 * 1024 * 1024


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
    if any(not isinstance(collection, list) for collection in (images, textures, materials, buffers)):
        raise TextureError("GLB содержит некорректные списки изображений, текстур или материалов.")
    if not images or not textures or not materials:
        raise TextureError("GLB создан, но в нём не найдены встроенные фототекстуры и материалы.")
    if any(not isinstance(item, dict) for item in [*images, *buffers]):
        raise TextureError("GLB содержит некорректные записи изображений или данных.")
    external_resources = [item.get("uri") for item in [*images, *buffers]
                          if item.get("uri") and not item["uri"].startswith("data:")]
    if external_resources:
        raise TextureError("GLB ссылается на внешние файлы и не является автономной моделью.")
    if any(not image.get("uri", "").startswith("data:") and "bufferView" not in image for image in images):
        raise TextureError("GLB содержит фототекстуру без встроенных данных.")
    return {"image_count": len(images), "texture_count": len(textures), "material_count": len(materials)}


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
    with tempfile.TemporaryDirectory(prefix=".spc-texture-", dir=output_path.parent) as temporary:
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
                "--output-file", str(output_base), "--export-type", "glb",
                "--max-threads", str(threads), "--working-folder", str(work)], work, cancel_file)
        candidate = output_base.with_suffix(".glb")
        glb = inspect_glb(candidate)
        _check_cancel(cancel_file)
        try:
            publish_without_replacement(candidate, output_path)
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
