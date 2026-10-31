#!/usr/bin/env python3
"""Experimental, local photo-to-PLY workflow; not the Spatial engine."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time


IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".tif", ".tiff", ".cr2"}
MODES = ("sparse-cpu", "dense-cpu", "dense-amd", "dense-opencl")


class ReconstructionError(Exception):
    pass


class ReconstructionCancelled(Exception):
    pass


def check_cancel(cancel_file: Path | None) -> None:
    if cancel_file is not None and cancel_file.exists():
        raise ReconstructionCancelled("Обработка остановлена пользователем.")


class CancellationWatch:
    def __init__(self, cancel_file: Path | None, pycolmap) -> None:
        self.cancel_file = cancel_file
        self.token = pycolmap.CancellationToken()
        self.done = threading.Event()
        self.thread = None

    def start(self) -> None:
        if self.cancel_file is None:
            return
        def watch() -> None:
            while not self.done.wait(0.1):
                if self.cancel_file is not None and self.cancel_file.exists():
                    self.token.cancel()
                    return
        self.thread = threading.Thread(target=watch, daemon=True)
        self.thread.start()

    def stop(self) -> None:
        self.done.set()
        if self.thread is not None:
            self.thread.join(timeout=1)


def image_paths(directory: Path) -> list[Path]:
    if not directory.is_dir():
        raise ReconstructionError(f"Папка фотографий не найдена: {directory}")
    paths = sorted(
        (p for p in directory.rglob("*") if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS),
        key=lambda p: p.relative_to(directory).as_posix(),
    )
    if len(paths) < 3:
        raise ReconstructionError("Нужно не менее 3 фотографий CR2, JPG, PNG или TIFF.")
    return paths


def validate_images(paths: list[Path], cancel_file: Path | None = None) -> None:
    from PIL import Image

    for path in paths:
        check_cancel(cancel_file)
        if path.suffix.lower() == ".cr2":
            # RAW files are fully decoded when the working images are prepared.
            continue
        try:
            with Image.open(path) as image:
                image.verify()
            with Image.open(path) as image:
                image.load()
        except Exception as exc:
            raise ReconstructionError(f"Не удалось прочитать изображение {path}: {exc}") from exc


def ply_vertex_count(path: Path) -> int:
    scalar_types = {
        "char": ("b", 1), "int8": ("b", 1), "uchar": ("B", 1), "uint8": ("B", 1),
        "short": ("h", 2), "int16": ("h", 2), "ushort": ("H", 2), "uint16": ("H", 2),
        "int": ("i", 4), "int32": ("i", 4), "uint": ("I", 4), "uint32": ("I", 4),
        "float": ("f", 4), "float32": ("f", 4), "double": ("d", 8), "float64": ("d", 8),
    }
    try:
        with path.open("rb") as stream:
            if stream.readline().strip() != b"ply":
                raise ValueError("не PLY")
            count, fmt, element = None, None, None
            properties = []
            for _ in range(10000):
                line = stream.readline(4096)
                if not line or len(line) == 4096:
                    break
                tokens = line.decode("ascii").split()
                if not tokens:
                    continue
                if tokens[0] == "format":
                    fmt = tokens[1]
                elif tokens[0] == "element":
                    element = tokens[1]
                    if element == "vertex":
                        if count is not None:
                            raise ValueError("повторная секция vertex")
                        count = int(tokens[2])
                    elif count is None and int(tokens[2]) > 0:
                        raise ValueError("данные перед vertex не поддерживаются")
                elif tokens[0] == "property" and element == "vertex":
                    if tokens[1] == "list":
                        count_type, item_type = tokens[2], tokens[3]
                        if count_type not in scalar_types or item_type not in scalar_types:
                            raise ValueError("неизвестный тип списка")
                        if scalar_types[count_type][0] in "fd":
                            raise ValueError("нецелый размер списка")
                        properties.append((count_type, item_type))
                    else:
                        if tokens[1] not in scalar_types:
                            raise ValueError("неизвестный тип свойства")
                        properties.append((None, tokens[1]))
                if line.strip() == b"end_header":
                    if count is None or count <= 0 or not properties:
                        raise ValueError("нет точек")
                    if fmt == "ascii":
                        for _ in range(count):
                            if not stream.readline().strip():
                                raise ValueError("файл обрезан внутри данных точек")
                    elif fmt in {"binary_little_endian", "binary_big_endian"}:
                        remaining = path.stat().st_size - stream.tell()
                        minimum = sum(scalar_types[a or b][1] for a, b in properties)
                        if count * minimum > remaining:
                            raise ValueError("файл обрезан внутри данных точек")
                        endian = "<" if fmt == "binary_little_endian" else ">"
                        if all(a is None for a, _ in properties):
                            stream.seek(count * minimum, os.SEEK_CUR)
                        else:
                            end = path.stat().st_size
                            for _ in range(count):
                                for count_type, value_type in properties:
                                    if count_type is None:
                                        stream.seek(scalar_types[value_type][1], os.SEEK_CUR)
                                        continue
                                    code, size = scalar_types[count_type]
                                    value = stream.read(size)
                                    if len(value) != size:
                                        raise ValueError("обрезанный размер списка")
                                    length = struct.unpack(endian + code, value)[0]
                                    skip = length * scalar_types[value_type][1]
                                    if length < 0 or stream.tell() + skip > end:
                                        raise ValueError("обрезанный список точек")
                                    stream.seek(skip, os.SEEK_CUR)
                            if stream.tell() > end:
                                raise ValueError("файл обрезан внутри данных точек")
                    else:
                        raise ValueError("неизвестный формат PLY")
                    return count
            raise ValueError("неполный заголовок")
    except (OSError, ValueError, IndexError, UnicodeError, struct.error) as exc:
        raise ReconstructionError(f"Некорректное или пустое облако {path}: {exc}") from exc


def executable(directory: Path, name: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    path = directory / (name + suffix)
    if not path.is_file() or (os.name != "nt" and not os.access(path, os.X_OK)):
        raise ReconstructionError(f"Не найдена программа {path}. Укажите папку с файлами OpenMVS.")
    return path.resolve()


def run_program(args: list[str], cwd: Path, cancel_file: Path | None = None) -> None:
    print("Запуск:", " ".join(args), flush=True)
    check_cancel(cancel_file)
    try:
        process = subprocess.Popen(args, cwd=cwd, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except OSError as exc:
        raise ReconstructionError(f"Не удалось запустить {args[0]}: {exc}") from exc
    while process.poll() is None:
        if cancel_file is not None and cancel_file.exists():
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            raise ReconstructionCancelled("Обработка остановлена пользователем.")
        time.sleep(0.1)
    check_cancel(cancel_file)
    if process.returncode != 0:
        raise ReconstructionError(f"{Path(args[0]).name} завершился с кодом {process.returncode}.")


def save_report(path: Path, report: dict) -> None:
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def reconstruct(images: Path, output: Path, mode: str, threads: int,
                openmvs_dir: Path | None = None, colmap: Path | None = None,
                cancel_file: Path | None = None) -> dict:
    try:
        import pycolmap
        from PIL import Image  # noqa: F401
    except ImportError as exc:
        raise ReconstructionError("Установите зависимости: python -m pip install -r requirements.txt") from exc
    if pycolmap.__version__ != "4.2.1":
        raise ReconstructionError(f"Нужен pycolmap 4.2.1; установлен {pycolmap.__version__}.")
    if mode not in MODES or threads < 1 or threads > 64:
        raise ReconstructionError("Некорректный режим или число потоков (1–64).")

    images = images.expanduser().resolve()
    output = output.expanduser().resolve()
    paths = image_paths(images)
    if output == images or output.is_relative_to(images):
        raise ReconstructionError("Папка результата должна находиться вне папки фотографий.")
    if output.exists():
        raise ReconstructionError(f"Папка результата уже существует: {output}")
    interface = densifier = None
    cl_backend = cl_environment = selected_gpu = None
    if mode == "dense-cpu":
        if openmvs_dir is None:
            raise ReconstructionError("Для плотного облака укажите папку с программами OpenMVS.")
        interface = executable(openmvs_dir.expanduser(), "InterfaceCOLMAP")
        densifier = executable(openmvs_dir.expanduser(), "DensifyPointCloud")
    if mode in {"dense-amd", "dense-opencl"}:
        if colmap is None or not colmap.expanduser().is_file():
            raise ReconstructionError("Укажите исполняемый файл подходящего движка COLMAP.")
        colmap = colmap.expanduser().resolve()
        if colmap.suffix.lower() in {".bat", ".cmd"}:
            raise ReconstructionError("Выберите исполняемый файл colmap.exe, а не .bat/.cmd.")
    if mode == "dense-opencl":
        import colmap_cl_backend as cl_backend
        cl_environment = cl_backend.backend_environment(colmap)
        cl_backend.verify_backend(colmap, cl_environment)
        selected_gpu = cl_backend.select_target_gpu()
        print(f"Выбрана OpenCL GPU: {selected_gpu['device']}, платформа/устройство "
              f"{selected_gpu['platform_index']}/{selected_gpu['device_index']} "
              f"(COLMAP-CL gpu_index={selected_gpu['colmap_gpu_index']}).", flush=True)

    print(f"Проверка {len(paths)} изображений...", flush=True)
    validate_images(paths, cancel_file)
    check_cancel(cancel_file)
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "status": "running", "mode": mode, "input_directory": str(images),
        "input_images": [p.relative_to(images).as_posix() for p in paths],
        "threads": threads, "pycolmap_version": pycolmap.__version__,
        "scale": "unknown", "started_unix_seconds": time.time(),
    }
    if selected_gpu is not None:
        report.update(backend="COLMAP-CL 1.5 (внешний, эксперимент)", opencl_device=selected_gpu,
                      opencl_device_verified=False, backend_log="colmap-cl.log")
    report_path = output / "report.json"
    save_report(report_path, report)
    watch = CancellationWatch(cancel_file, pycolmap)
    watch.start()
    try:
        processing_images = images
        image_names = report["input_images"]
        if any(path.suffix.lower() == ".cr2" for path in paths):
            from raw_import import prepare_images
            print("Подготовка рабочих копий CR2 на CPU; исходные файлы не меняются…", flush=True)
            processing_images, image_map = prepare_images(paths, images, output, lambda: check_cancel(cancel_file))
            image_names = [entry["prepared"] for entry in image_map]
            report.update(prepared_directory="prepared_images", image_mapping=image_map)
            save_report(report_path, report)
        db, sparse = output / "database.db", output / "sparse"
        sparse.mkdir()
        extraction = pycolmap.FeatureExtractionOptions()
        extraction.num_threads, extraction.max_image_size, extraction.use_gpu = threads, 2000, False
        matching = pycolmap.FeatureMatchingOptions()
        matching.num_threads, matching.use_gpu = threads, False
        mapping = pycolmap.IncrementalPipelineOptions()
        mapping.num_threads, mapping.min_model_size = threads, min(10, len(paths))
        print("1/4. Поиск особенностей на фотографиях (CPU)...", flush=True)
        pycolmap.extract_features(db, processing_images, image_names=image_names,
                                  extraction_options=extraction, device=pycolmap.Device.cpu,
                                  cancellation_token=watch.token)
        check_cancel(cancel_file)
        print("2/4. Сопоставление фотографий (CPU)...", flush=True)
        pycolmap.match_exhaustive(db, matching_options=matching, device=pycolmap.Device.cpu,
                                 cancellation_token=watch.token)
        check_cancel(cancel_file)
        print("3/4. Построение камер и разреженного облака...", flush=True)
        models = pycolmap.incremental_mapping(db, images, sparse, options=mapping, cancellation_token=watch.token)
        check_cancel(cancel_file)
        if not models:
            raise ReconstructionError("Не удалось связать фотографии. Проверьте перекрытие и текстуру сцены.")
        model_id, model = max(models.items(), key=lambda item: (item[1].num_reg_images(), item[1].num_points3D()))
        if model.num_reg_images() < 2 or model.num_points3D() < 1:
            raise ReconstructionError("COLMAP не построил пригодную модель с точками.")
        sparse_ply = output / "sparse.ply"
        model.export_PLY(sparse_ply)
        report.update(registered_images=model.num_reg_images(), sparse_points=ply_vertex_count(sparse_ply),
                      models=len(models), selected_model=int(model_id))
        if len(models) > 1:
            print(f"Предупреждение: найдено моделей: {len(models)}; выбрана модель с большим числом камер.", flush=True)
        result_path = sparse_ply
        if mode != "sparse-cpu":
            print("4/4. Подготовка и построение плотного облака...", flush=True)
            workspace = output / "dense_workspace"
            pycolmap.undistort_images(workspace, sparse / str(model_id), processing_images, num_threads=threads,
                                     cancellation_token=watch.token)
            check_cancel(cancel_file)
            # Models can contain only a subset; validate the images COLMAP wrote.
            prepared = [p for p in (workspace / "images").rglob("*") if p.is_file()]
            if len(prepared) != model.num_reg_images():
                raise ReconstructionError("COLMAP подготовил не все изображения для плотной реконструкции.")
            validate_images(prepared, cancel_file)
            if mode == "dense-cpu":
                scene, dense_scene = output / "scene.mvs", output / "dense.mvs"
                run_program([str(interface), "-i", str(workspace), "-o", str(scene),
                             "--image-folder", str(workspace / "images"), "--max-threads", str(threads)], output, cancel_file)
                run_program([str(densifier), "-i", str(scene), "-o", str(dense_scene),
                             "--max-threads", str(threads), "--resolution-level", "2",
                             "--min-resolution", "320", "--max-resolution", "1280"], output, cancel_file)
                result_path = output / "dense.ply"
            elif mode == "dense-opencl":
                log = output / "colmap-cl.log"
                run_cl = lambda args, expected=None: cl_backend.run_program(
                    args, output, cl_environment, log, lambda: check_cancel(cancel_file), expected)
                print("Плотная реконструкция на выбранной OpenCL GPU (эксперимент)…", flush=True)
                run_cl([str(colmap), "patch_match_stereo", "--workspace_path", str(workspace),
                        "--workspace_format", "COLMAP", "--PatchMatchStereo.max_image_size", "1280",
                        "--PatchMatchStereo.num_iterations", "3", "--PatchMatchStereo.geom_consistency", "0",
                        "--PatchMatchStereo.gpu_index", str(selected_gpu["colmap_gpu_index"])], selected_gpu)
                report["opencl_device_verified"] = True
                save_report(report_path, report)
                result_path = output / "dense.ply"
                run_cl([str(colmap), "stereo_fusion", "--workspace_path", str(workspace),
                        "--workspace_format", "COLMAP", "--input_type", "photometric",
                        "--output_path", str(result_path)])
            else:
                print("Режим AMD требует рабочей HIP/ROCm-сборки COLMAP; совместимость карты не проверена.", flush=True)
                run_program([str(colmap), "patch_match_stereo", "--workspace_path", str(workspace),
                             "--workspace_format", "COLMAP", "--PatchMatchStereo.max_image_size", "1600"], output, cancel_file)
                result_path = output / "dense.ply"
                run_program([str(colmap), "stereo_fusion", "--workspace_path", str(workspace),
                             "--workspace_format", "COLMAP", "--input_type", "geometric",
                             "--output_path", str(result_path)], output, cancel_file)
        else:
            print("4/4. Сохранение разреженного облака...", flush=True)
        check_cancel(cancel_file)
        point_count = ply_vertex_count(result_path)
        check_cancel(cancel_file)
        try:
            from preview import create_preview
            preview_count = create_preview(result_path, output / "preview.html")
            report.update(preview="preview.html", preview_points=preview_count)
        except Exception as exc:
            print(f"Не удалось создать предварительный просмотр: {exc}", flush=True)
            report["preview_error"] = str(exc)
        check_cancel(cancel_file)
        report.update(status="succeeded", point_cloud=result_path.name,
                      point_count=point_count, finished_unix_seconds=time.time())
        save_report(report_path, report)
        print(f"Готово: {result_path} ({report['point_count']} точек). Масштаб не определён.", flush=True)
        return report
    except Exception as exc:
        if cancel_file is not None and cancel_file.exists():
            exc = ReconstructionCancelled("Обработка остановлена пользователем.")
        report.update(status="cancelled" if isinstance(exc, ReconstructionCancelled) else "failed",
                      error=str(exc), finished_unix_seconds=time.time())
        save_report(report_path, report)
        raise exc
    finally:
        watch.stop()


def main() -> int:
    parser = argparse.ArgumentParser(description="Экспериментальное локальное построение облака из фотографий")
    parser.add_argument("--images", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="Новая папка результата")
    parser.add_argument("--mode", choices=MODES, default="sparse-cpu")
    parser.add_argument("--threads", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--openmvs-dir", type=Path)
    parser.add_argument("--colmap", type=Path)
    parser.add_argument("--cancel-file", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        reconstruct(args.images, args.output, args.mode, args.threads, args.openmvs_dir, args.colmap, args.cancel_file)
        return 0
    except ReconstructionCancelled as exc:
        print(str(exc), file=sys.stderr, flush=True)
        return 130
    except Exception as exc:
        print(f"Ошибка: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
