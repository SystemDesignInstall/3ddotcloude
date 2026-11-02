#!/usr/bin/env python3
"""Build a colored Poisson triangle mesh from a COLMAP dense PLY point cloud."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
import time
import uuid


DEFAULT_DEPTH = 12
MAX_DEPTH = 15
MAX_THREADS = 64


class MeshError(RuntimeError):
    pass


def publish_without_replacement(source: Path, target: Path) -> None:
    """Publish one completed file atomically and fail if its name is occupied."""
    if os.name == "nt":
        # Windows rename refuses to replace an existing target.
        os.rename(source, target)
    else:
        # POSIX rename replaces an existing target, so use an exclusive link.
        os.link(source, target)
        try:
            source.unlink()
        except OSError:
            pass


def inspect_input(path: Path) -> dict:
    """Read the PLY header and require the oriented normals Poisson needs."""
    try:
        with path.open("rb") as stream:
            header = stream.read(1024 * 1024)
    except OSError as exc:
        raise MeshError(f"Не удалось прочитать PLY: {exc}") from exc
    marker = re.search(rb"(?:^|\n)end_header[ \t]*\r?\n", header)
    if marker is None:
        raise MeshError("Заголовок PLY не найден или превышает 1 МБ.")
    try:
        lines = header[:marker.end()].decode("ascii").splitlines()
    except UnicodeDecodeError as exc:
        raise MeshError("Заголовок PLY должен содержать ASCII-текст.") from exc
    if not lines or lines[0].strip() != "ply":
        raise MeshError("Выбранный файл не является PLY.")

    current = None
    vertex_count = face_count = 0
    vertex_props: set[str] = set()
    format_name = None
    metric = False
    up_axis = "y"
    for line in lines:
        parts = line.strip().split()
        if not parts:
            continue
        if parts[0] == "format" and len(parts) >= 2:
            format_name = parts[1]
        elif parts[0] == "comment" and parts[1:] == ["SpatialPhotoCloud", "units", "meters"]:
            metric = True
        elif parts[0] == "comment" and parts[1:] == ["SpatialPhotoCloud", "up", "z"]:
            up_axis = "z"
        elif parts[0] == "element" and len(parts) == 3:
            try:
                count = int(parts[2])
            except ValueError as exc:
                raise MeshError("Некорректное количество элементов PLY.") from exc
            if count < 0:
                raise MeshError("Количество элементов PLY не может быть отрицательным.")
            current = parts[1]
            if current == "vertex":
                vertex_count = count
            elif current == "face":
                face_count = count
        elif parts[0] == "property" and current == "vertex":
            if len(parts) == 3 and parts[1] != "list":
                vertex_props.add(parts[2])

    if format_name not in {"ascii", "binary_little_endian", "binary_big_endian"}:
        raise MeshError(f"Неподдерживаемый формат PLY: {format_name or 'не указан'}.")
    if vertex_count < 3:
        raise MeshError("Для сетки нужны как минимум три точки.")
    if face_count:
        raise MeshError("Выберите исходное облако точек без граней, а не готовую 3D-сетку.")
    if not {"x", "y", "z"}.issubset(vertex_props):
        raise MeshError("В PLY отсутствуют координаты x, y, z.")
    if not {"nx", "ny", "nz"}.issubset(vertex_props):
        raise MeshError("В PLY нет нормалей nx, ny, nz. Сетка строится из плотного COLMAP-облака с нормалями.")
    return {
        "vertex_count": vertex_count,
        "face_count": face_count,
        "has_normals": True,
        "has_colors": {"red", "green", "blue"}.issubset(vertex_props)
        or {"r", "g", "b"}.issubset(vertex_props)
        or {"diffuse_red", "diffuse_green", "diffuse_blue"}.issubset(vertex_props),
        "scale": "meters" if metric else "unknown",
        "up_axis": up_axis,
    }


def inspect_mesh(path: Path) -> dict:
    """Require a non-empty triangle mesh output, rather than accepting a PLY cloud."""
    if not path.is_file() or path.stat().st_size <= 0:
        raise MeshError("Движок не создал файл сетки.")
    try:
        with path.open("rb") as stream:
            data = stream.read(1024 * 1024)
    except OSError as exc:
        raise MeshError(f"Не удалось проверить созданную сетку: {exc}") from exc
    marker = re.search(rb"(?:^|\n)end_header[ \t]*\r?\n", data)
    if marker is None:
        raise MeshError("Созданный PLY имеет неполный заголовок.")
    header = data[:marker.end()].decode("ascii", errors="replace")
    vertices = faces = 0
    for line in header.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0] == "element":
            if parts[1] == "vertex":
                vertices = int(parts[2])
            elif parts[1] == "face":
                faces = int(parts[2])
    if vertices < 3 or faces < 1:
        raise MeshError("Файл результата не содержит треугольную поверхность.")
    if path.stat().st_size <= marker.end():
        raise MeshError("Файл сетки не содержит данных после заголовка.")
    return {"vertex_count": vertices, "face_count": faces}


def preserve_spatial_metadata(path: Path, metadata: dict) -> None:
    """Keep Spatial Photo Cloud's metric and vertical-axis hints in the new PLY."""
    additions = []
    if metadata["scale"] == "meters":
        additions.append(b"comment SpatialPhotoCloud units meters\n")
    if metadata["up_axis"] == "z":
        additions.append(b"comment SpatialPhotoCloud up z\n")
    if not additions:
        return
    try:
        with path.open("rb") as source:
            header = source.read(1024 * 1024)
        marker = re.search(rb"end_header[ \t]*\r?\n", header)
        if marker is None:
            raise MeshError("Заголовок созданной сетки не найден или превышает 1 МБ.")
        header_text = header[:marker.end()]
        additions = [line for line in additions if line not in header_text]
        if not additions:
            return
        # Insert comments before end_header while copying the potentially large
        # vertex and face payload in bounded chunks.
        replacement = path.with_name(f".{path.name}.{uuid.uuid4().hex}.metadata")
        try:
            with path.open("rb") as source, replacement.open("wb") as target:
                target.write(header[:marker.start()])
                target.writelines(additions)
                target.write(header[marker.start():])
                source.seek(len(header))
                while chunk := source.read(1024 * 1024):
                    target.write(chunk)
            os.replace(replacement, path)
        finally:
            replacement.unlink(missing_ok=True)
    except OSError as exc:
        raise MeshError(f"Не удалось сохранить метаданные сетки: {exc}") from exc


def build_mesh(input_path: Path, output_path: Path, threads: int,
               cancel_file: Path | None = None, depth: int = DEFAULT_DEPTH) -> dict:
    input_path = input_path.expanduser().resolve(strict=True)
    output_path = output_path.expanduser().resolve()
    if not input_path.is_file() or input_path.suffix.casefold() != ".ply":
        raise MeshError("Выберите существующий файл PLY.")
    if output_path.suffix.casefold() != ".ply":
        raise MeshError("Для результата сетки выберите расширение .ply.")
    if input_path == output_path:
        raise MeshError("Сетка должна сохраняться в отдельный файл; исходный PLY не изменён.")
    if not output_path.parent.is_dir():
        raise MeshError("Папка для результата не существует.")
    report_path = output_path.with_name(output_path.name + ".report.json")
    if output_path.exists():
        raise MeshError("Файл результата уже существует. Выберите другое имя; существующий файл не изменён.")
    if report_path.exists():
        raise MeshError("Отчёт для этого результата уже существует. Выберите другое имя; файлы не изменены.")
    if not 1 <= threads <= MAX_THREADS:
        raise MeshError(f"Число потоков должно быть от 1 до {MAX_THREADS}.")
    if not 8 <= depth <= MAX_DEPTH:
        raise MeshError(f"Глубина Poisson должна быть от 8 до {MAX_DEPTH}.")
    metadata = inspect_input(input_path)
    if cancel_file is not None and cancel_file.exists():
        raise MeshError("Построение сетки отменено.")

    try:
        import pycolmap
    except ImportError as exc:
        raise MeshError("В этой сборке не найден PyCOLMAP. Пересоберите программу из полного пакета.") from exc
    mesher = getattr(pycolmap, "poisson_meshing", None)
    options_type = getattr(pycolmap, "PoissonMeshingOptions", None)
    if mesher is None or options_type is None:
        raise MeshError(
            f"PyCOLMAP {getattr(pycolmap, '__version__', 'неизвестной версии')} "
            "не содержит Poisson meshing. Нужна сборка COLMAP 4.2.1."
        )

    job_id = cancel_file.parent.name if cancel_file is not None else uuid.uuid4().hex
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", job_id):
        job_id = uuid.uuid4().hex
    temporary = output_path.with_name(f".{output_path.stem}.{job_id}.partial.ply")
    report_temporary = report_path.with_name(f".{report_path.name}.{job_id}.partial")
    report_published = False
    output_published = False
    started = time.time()
    print(f"Входное облако: {input_path.name} ({metadata['vertex_count']:,} точек).", flush=True)
    print(f"Построение цветной поверхности Poisson: глубина {depth}, CPU-потоки {threads}…", flush=True)
    print("Этап выполняется на CPU; исходный PLY останется без изменений.", flush=True)
    try:
        options = options_type()
        options.depth = depth
        options.trim = 10.0
        options.num_threads = threads
        options.color = bool(metadata["has_colors"])
        mesher(input_path=str(input_path), output_path=str(temporary), options=options)
        if cancel_file is not None and cancel_file.exists():
            raise MeshError("Построение сетки отменено.")
        preserve_spatial_metadata(temporary, metadata)
        mesh = inspect_mesh(temporary)
        report = {
            "status": "succeeded",
            "operation": "poisson_surface_reconstruction",
            "backend": "PyCOLMAP",
            "backend_version": str(pycolmap.__version__),
            "input": input_path.name,
            "output": output_path.name,
            "input_points": metadata["vertex_count"],
            "mesh_vertices": mesh["vertex_count"],
            "mesh_faces": mesh["face_count"],
            "scale": metadata["scale"],
            "depth": depth,
            "threads": threads,
            "up_axis": metadata["up_axis"],
            "compute_backend": "CPU",
            "elapsed_seconds": round(time.time() - started, 3),
            "limitations": [
                "Poisson can close holes and create surfaces across open or weakly observed regions.",
                "No photo texture atlas is generated by this stage.",
            ],
        }
        with report_temporary.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        try:
            publish_without_replacement(report_temporary, report_path)
        except FileExistsError as exc:
            raise MeshError("Отчёт для этого результата уже существует; файлы не изменены.") from exc
        except OSError as exc:
            raise MeshError(f"Не удалось сохранить отчёт сетки: {exc}") from exc
        report_published = True
        try:
            publish_without_replacement(temporary, output_path)
        except FileExistsError as exc:
            report_path.unlink(missing_ok=True)
            report_published = False
            raise MeshError("Файл результата появился во время построения; существующий файл сохранён.") from exc
        except OSError as exc:
            raise MeshError(f"Не удалось сохранить сетку: {exc}") from exc
        output_published = True
    except Exception as exc:
        if report_published and not output_published:
            report_path.unlink(missing_ok=True)
        if isinstance(exc, MeshError):
            raise
        raise MeshError(f"PyCOLMAP не смог построить сетку: {exc}") from exc
    finally:
        for path in (temporary, report_temporary):
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass
    print(f"Готово: {output_path.name}; вершин {mesh['vertex_count']:,}, "
          f"граней {mesh['face_count']:,}; масштаб: {metadata['scale']}.", flush=True)
    print(f"Отчёт сетки: {report_path.name}", flush=True)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="Построение цветной 3D-сетки из PLY")
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--depth", type=int, default=DEFAULT_DEPTH)
    parser.add_argument("--cancel-file", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        build_mesh(args.input, args.output, args.threads, args.cancel_file, args.depth)
        return 0
    except Exception as exc:
        print(f"Ошибка построения 3D-сетки: {exc}", file=sys.stderr, flush=True)
        return 130 if "отменено" in str(exc).casefold() else 1


if __name__ == "__main__":
    raise SystemExit(main())
