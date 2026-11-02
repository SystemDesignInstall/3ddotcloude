#!/usr/bin/env python3
"""Experimental, local photo-to-PLY workflow; not the Spatial engine."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import sys
import threading
import time

from hardware_profile import recommended_threads as hardware_recommended_threads
from hardware_profile import (
    cpu_model_name,
    logical_processor_count,
    physical_processor_count,
    total_physical_memory_bytes,
)


IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".tif", ".tiff", ".cr2"}
MODES = ("sparse-cpu", "dense-cpu", "dense-amd", "dense-opencl")
MAX_THREADS = 64
QUALITY_WARN_REGISTERED_RATIO = 0.80
QUALITY_REJECT_REGISTERED_RATIO = 0.50
QUALITY_WARN_MEAN_REPROJECTION_ERROR_PX = 2.5
QUALITY_REJECT_MEAN_REPROJECTION_ERROR_PX = 8.0


def recommended_threads(cpu_count: int | None = None, memory_bytes: int | None = None,
                        physical_count: int | None = None) -> int:
    """Return a CPU and RAM-aware worker default, capped for memory bandwidth."""
    return hardware_recommended_threads(cpu_count, memory_bytes, physical_count)


def verified_match_statistics(database: Path) -> dict:
    """Count geometrically verified image pairs from COLMAP's database.

    COLMAP reconstructs from ``two_view_geometries`` rather than raw descriptor
    matches, so these counts are a useful, concrete overlap diagnostic.
    """
    try:
        connection = sqlite3.connect(database)
        try:
            row = connection.execute(
                "SELECT COUNT(*), COALESCE(SUM(rows), 0) "
                "FROM two_view_geometries WHERE rows > 0"
            ).fetchone()
        finally:
            connection.close()
    except sqlite3.Error as exc:
        raise ReconstructionError(f"Не удалось прочитать проверенные пересечения COLMAP: {exc}") from exc
    return {"verified_image_pairs": int(row[0]), "verified_inlier_matches": int(row[1])}


def _copy_sqlite_database(source_path: Path, destination_path: Path) -> None:
    source = sqlite3.connect(source_path)
    destination = sqlite3.connect(destination_path)
    try:
        source.backup(destination)
    finally:
        destination.close()
        source.close()


def _restore_sqlite_database(backup_path: Path, database_path: Path) -> None:
    for suffix in ("-wal", "-shm"):
        Path(str(database_path) + suffix).unlink(missing_ok=True)
    shutil.copyfile(backup_path, database_path)


def match_with_opencl_fallback(database: Path, opencl_match, cpu_match, logger=print) -> dict:
    """Try the verified COLMAP-CL matcher, restoring the database before CPU fallback."""
    backup = database.with_name(database.name + ".before-opencl-matching")
    backup.unlink(missing_ok=True)
    try:
        _copy_sqlite_database(database, backup)
        try:
            opencl_match()
            statistics = verified_match_statistics(database)
            if statistics["verified_image_pairs"] < 1:
                raise RuntimeError("OpenCL-сопоставление не создало геометрически подтверждённых пар.")
            return {"status": "verified", "backend": "COLMAP-CL OpenCL", "statistics": statistics}
        except ReconstructionCancelled:
            _restore_sqlite_database(backup, database)
            raise
        except Exception as error:
            _restore_sqlite_database(backup, database)
            error_lines = str(error).splitlines()
            reason = (error_lines[0] if error_lines else type(error).__name__)[:500]
            logger("OpenCL-сопоставление не принято (" + reason + "); восстановлена база, запуск на CPU.")
            cpu_match()
            return {"status": "cpu_fallback", "backend": "CPU после OpenCL fallback",
                    "fallback_reason": reason, "statistics": verified_match_statistics(database)}
    finally:
        backup.unlink(missing_ok=True)


def source_image_name(model_name: str, image_mapping: list[dict] | None = None) -> str:
    """Translate a prepared CR2 working-copy name back to its source filename."""
    for entry in image_mapping or []:
        if entry.get("prepared") == model_name:
            return str(entry.get("source", model_name))
    return model_name


def _percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    offset = (len(ordered) - 1) * fraction
    lower = int(offset)
    upper = min(lower + 1, len(ordered) - 1)
    blend = offset - lower
    return ordered[lower] * (1.0 - blend) + ordered[upper] * blend


def _finite_float(value) -> float | None:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def assess_reconstruction_quality(model, input_names: list[str], verified_matches: dict,
                                  model_count: int, image_mapping: list[dict] | None = None,
                                  all_model_image_names: set[str] | None = None) -> tuple[dict, list[dict]]:
    """Summarize COLMAP consistency metrics and decide whether dense processing is safe to try.

    This is a triage gate, not proof that camera poses are physically correct.
    Without a known trajectory, GPS/IMU, control points, or measured scene
    dimensions, absolute pose correctness cannot be established from the model
    alone.
    """
    registered_ids = list(model.reg_image_ids())
    registered_images = [model.image(image_id) for image_id in registered_ids]
    registered_names = {
        source_image_name(image.name, image_mapping) for image in registered_images
    }
    registered_all_models = set(all_model_image_names or registered_names)
    unregistered = sorted(set(input_names) - registered_all_models)
    other_component_images = sorted(registered_all_models - registered_names)
    outside_selected_model = sorted(set(input_names) - registered_names)
    input_count = len(input_names)
    registered_count = len(registered_ids)
    registration_ratio = registered_count / input_count if input_count else 0.0

    try:
        mean_reprojection = _finite_float(model.compute_mean_reprojection_error())
    except (AttributeError, TypeError, ValueError):
        mean_reprojection = None
    point_errors = []
    for point in model.points3D.values():
        try:
            error = _finite_float(point.error)
            if error is not None and error >= 0:
                point_errors.append(error)
        except (AttributeError, TypeError, ValueError):
            continue
    if mean_reprojection is None and point_errors:
        mean_reprojection = sum(point_errors) / len(point_errors)
    try:
        mean_track_length = _finite_float(model.compute_mean_track_length())
    except (AttributeError, TypeError, ValueError):
        mean_track_length = None
    try:
        mean_observations = _finite_float(model.compute_mean_observations_per_reg_image())
    except (AttributeError, TypeError, ValueError):
        mean_observations = None

    messages = []
    rejected_reasons = []
    if verified_matches.get("verified_image_pairs", 0) == 0:
        rejected_reasons.append("COLMAP не нашёл ни одной геометрически подтверждённой пары фотографий.")
    if registered_count < 3:
        rejected_reasons.append("В модель вошло меньше трёх фотографий; плотная реконструкция ненадёжна.")
    if int(model.num_points3D()) < 1:
        rejected_reasons.append("В модели не построено ни одной 3D-точки.")
    if registration_ratio < QUALITY_REJECT_REGISTERED_RATIO:
        rejected_reasons.append(
            f"В выбранную модель вошло только {registration_ratio:.0%} исходных фотографий "
            f"(порог проверки — {QUALITY_REJECT_REGISTERED_RATIO:.0%})."
        )
    elif registration_ratio < QUALITY_WARN_REGISTERED_RATIO:
        messages.append(
            f"В выбранную модель вошло {registration_ratio:.0%} фотографий; проверьте список исключённых кадров."
        )
    if mean_reprojection is not None:
        if mean_reprojection > QUALITY_REJECT_MEAN_REPROJECTION_ERROR_PX:
            rejected_reasons.append(
                f"Средняя ошибка репроекции {mean_reprojection:.2f} px слишком велика "
                f"(порог проверки — {QUALITY_REJECT_MEAN_REPROJECTION_ERROR_PX:.1f} px)."
            )
        elif mean_reprojection > QUALITY_WARN_MEAN_REPROJECTION_ERROR_PX:
            messages.append(f"Повышенная средняя ошибка репроекции: {mean_reprojection:.2f} px.")
    else:
        messages.append("COLMAP не предоставил измеримую ошибку репроекции.")
    if mean_track_length is not None and mean_track_length < 2.5:
        messages.append(f"Низкая средняя длина треков 3D-точек: {mean_track_length:.2f} наблюдения.")
    if model_count > 1:
        messages.append(f"Найдены отдельные компоненты сцены: {model_count}; выбранная модель не включает остальные.")
    if unregistered:
        messages.append(f"Не зарегистрировано ни в одной компоненте фотографий: {len(unregistered)}.")

    # These thresholds are conservative product heuristics, not COLMAP's
    # definition of a correct reconstruction. Keep the rationale explicit.
    can_build_dense = not rejected_reasons
    messages = rejected_reasons + messages
    quality = {
        "assessment": "rejected" if rejected_reasons else ("review" if messages else "passed"),
        "dense_reconstruction_allowed": can_build_dense,
        "input_images": input_count,
        "registered_images": registered_count,
        "registered_fraction": round(registration_ratio, 6),
        "unregistered_images": unregistered,
        "images_outside_selected_model": outside_selected_model,
        "images_in_other_models": other_component_images,
        "model_components": model_count,
        "sparse_points": int(model.num_points3D()),
        "verified_image_pairs": int(verified_matches.get("verified_image_pairs", 0)),
        "verified_inlier_matches": int(verified_matches.get("verified_inlier_matches", 0)),
        "mean_reprojection_error_px": round(mean_reprojection, 6) if mean_reprojection is not None else None,
        "p95_point_reprojection_error_px": round(_percentile(point_errors, 0.95), 6)
        if point_errors else None,
        "mean_track_length": round(mean_track_length, 6) if mean_track_length is not None else None,
        "mean_observations_per_registered_image": round(mean_observations, 6)
        if mean_observations is not None else None,
        "messages": messages,
        "thresholds": {
            "warn_registered_fraction_below": QUALITY_WARN_REGISTERED_RATIO,
            "reject_dense_registered_fraction_below": QUALITY_REJECT_REGISTERED_RATIO,
            "warn_mean_reprojection_error_px_above": QUALITY_WARN_MEAN_REPROJECTION_ERROR_PX,
            "reject_dense_mean_reprojection_error_px_above": QUALITY_REJECT_MEAN_REPROJECTION_ERROR_PX,
            "type": "heuristic_triage_not_ground_truth_validation",
        },
        "absolute_camera_pose_validation": "requires_external_reference_or_control_measurements",
        "camera_centers_file": "camera_centers.csv",
    }

    pose_rows = []
    for image in registered_images:
        try:
            center = [_finite_float(value) for value in image.projection_center()]
            direction = [_finite_float(value) for value in image.viewing_direction()]
        except (AttributeError, TypeError, ValueError):
            center = [None, None, None]
            direction = [None, None, None]
        pose_rows.append({
            "image": source_image_name(image.name, image_mapping),
            "center": center,
            "direction": direction,
            "keypoints": int(image.num_points2D()),
            "triangulated_observations": int(image.num_points3D),
        })
    return quality, pose_rows


def write_camera_centers(path: Path, pose_rows: list[dict]) -> None:
    """Write relative COLMAP camera centers/directions for visual inspection."""
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("image", "x", "y", "z", "view_x", "view_y", "view_z",
                         "keypoints", "triangulated_observations"))
        for row in pose_rows:
            writer.writerow((row["image"], *row["center"], *row["direction"],
                             row["keypoints"], row["triangulated_observations"]))


def failed_reconstruction_quality(input_names: list[str], verified_matches: dict,
                                  message: str) -> dict:
    return {
        "assessment": "rejected",
        "dense_reconstruction_allowed": False,
        "input_images": len(input_names),
        "registered_images": 0,
        "registered_fraction": 0.0,
        "unregistered_images": sorted(input_names),
        "images_outside_selected_model": sorted(input_names),
        "images_in_other_models": [],
        "model_components": 0,
        "sparse_points": 0,
        "verified_image_pairs": int(verified_matches.get("verified_image_pairs", 0)),
        "verified_inlier_matches": int(verified_matches.get("verified_inlier_matches", 0)),
        "mean_reprojection_error_px": None,
        "p95_point_reprojection_error_px": None,
        "mean_track_length": None,
        "mean_observations_per_registered_image": None,
        "messages": [message],
        "thresholds": {
            "warn_registered_fraction_below": QUALITY_WARN_REGISTERED_RATIO,
            "reject_dense_registered_fraction_below": QUALITY_REJECT_REGISTERED_RATIO,
            "warn_mean_reprojection_error_px_above": QUALITY_WARN_MEAN_REPROJECTION_ERROR_PX,
            "reject_dense_mean_reprojection_error_px_above": QUALITY_REJECT_MEAN_REPROJECTION_ERROR_PX,
            "type": "heuristic_triage_not_ground_truth_validation",
        },
        "absolute_camera_pose_validation": "requires_external_reference_or_control_measurements",
        "camera_centers_file": None,
    }


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
    if mode not in MODES or threads < 1 or threads > MAX_THREADS:
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
    # Report input, destination and engine problems before requiring the heavy
    # reconstruction wheel. This also keeps the output folder untouched when a
    # required runtime dependency is missing.
    try:
        import pycolmap
        from PIL import Image  # noqa: F401
    except ImportError as exc:
        raise ReconstructionError("Установите зависимости: python -m pip install -r requirements.txt") from exc
    if pycolmap.__version__ != "4.2.1":
        raise ReconstructionError(f"Нужен pycolmap 4.2.1; установлен {pycolmap.__version__}.")
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "status": "running", "mode": mode, "input_directory": str(images),
        "input_images": [p.relative_to(images).as_posix() for p in paths],
        "threads": threads, "cpu_logical_threads": logical_processor_count(),
        "hardware": {
            "cpu_model": cpu_model_name(),
            "logical_processors": logical_processor_count(),
            "physical_cores": physical_processor_count(logical_processor_count()),
            "total_memory_bytes": total_physical_memory_bytes(),
            "recommended_threads": recommended_threads(),
            "selected_threads": threads,
            "selected_mode": mode,
        },
        "pycolmap_version": pycolmap.__version__,
        "scale": "unknown", "started_unix_seconds": time.time(),
        "compute_backends": {
            "cr2_conversion": "not required",
            "feature_extraction": "CPU",
            "feature_matching": "CPU",
            "camera_reconstruction": "CPU",
            "dense_stereo": "not run",
            "stereo_fusion": "not run",
        },
    }
    if selected_gpu is not None:
        report.update(backend="COLMAP-CL 1.5 (внешний, эксперимент)", opencl_device=selected_gpu,
                      opencl_device_verified=False, backend_log="colmap-cl.log", opencl_stages={})
    report_path = output / "report.json"
    save_report(report_path, report)
    watch = CancellationWatch(cancel_file, pycolmap)
    watch.start()
    colmap_cl_log = output / "colmap-cl.log"

    def run_cl(args, expected=None):
        if cl_backend is None or cl_environment is None:
            raise ReconstructionError("Среда COLMAP-CL не настроена.")
        return cl_backend.run_program(args, output, cl_environment, colmap_cl_log,
                                      lambda: check_cancel(cancel_file), expected)

    try:
        processing_images = images
        image_names = report["input_images"]
        if any(path.suffix.lower() == ".cr2" for path in paths):
            from raw_import import prepare_images
            print("Подготовка рабочих копий CR2 на CPU; исходные файлы не меняются…", flush=True)
            processing_images, image_map = prepare_images(paths, images, output, lambda: check_cancel(cancel_file))
            image_names = [entry["prepared"] for entry in image_map]
            report.update(prepared_directory="prepared_images", image_mapping=image_map)
            report["compute_backends"]["cr2_conversion"] = "CPU"
            save_report(report_path, report)
        db, sparse = output / "database.db", output / "sparse"
        sparse.mkdir()
        extraction = pycolmap.FeatureExtractionOptions()
        extraction.num_threads, extraction.max_image_size, extraction.use_gpu = threads, 2000, False
        matching = pycolmap.FeatureMatchingOptions()
        matching.num_threads, matching.use_gpu = threads, False
        mapping = pycolmap.IncrementalPipelineOptions()
        mapping.num_threads, mapping.min_model_size = threads, min(10, len(paths))
        print(f"Параллелизм CPU: {threads} потоков (доступно логических: {logical_processor_count()}).", flush=True)
        print("1/4. Поиск особенностей на фотографиях (CPU)...", flush=True)
        pycolmap.extract_features(db, processing_images, image_names=image_names,
                                  extraction_options=extraction, device=pycolmap.Device.cpu,
                                  cancellation_token=watch.token)
        check_cancel(cancel_file)
        def match_on_cpu():
            pycolmap.match_exhaustive(db, matching_options=matching, device=pycolmap.Device.cpu,
                                      cancellation_token=watch.token)

        if mode == "dense-opencl":
            print("2/4. Сопоставление фотографий на AMD OpenCL (эксперимент; при сбое — CPU)...", flush=True)
            opencl_command = [str(colmap), "exhaustive_matcher", "--database_path", str(db),
                              "--SiftMatching.use_gpu", "1", "--SiftMatching.num_threads", str(threads),
                              "--SiftMatching.gpu_index",
                              str(selected_gpu["colmap_gpu_index"])]
            match_result = match_with_opencl_fallback(
                db, lambda: run_cl(opencl_command, selected_gpu), match_on_cpu,
                logger=lambda message: print(message, flush=True))
            match_stats = match_result["statistics"]
            report["compute_backends"]["feature_matching"] = match_result["backend"]
            report["opencl_stages"]["sift_matching"] = {
                "status": match_result["status"], "backend": match_result["backend"],
            }
            if match_result["status"] == "verified":
                report["opencl_stages"]["sift_matching"]["device"] = selected_gpu["device"]
                report["opencl_device_verified"] = True
            elif match_result.get("fallback_reason"):
                report["opencl_stages"]["sift_matching"]["fallback_reason"] = match_result["fallback_reason"]
        else:
            print("2/4. Сопоставление фотографий (CPU)...", flush=True)
            match_on_cpu()
            match_stats = verified_match_statistics(db)
        check_cancel(cancel_file)
        report["verified_matches"] = match_stats
        print(f"Геометрически подтверждённые пересечения: {match_stats['verified_image_pairs']} пар, "
              f"{match_stats['verified_inlier_matches']} соответствий.", flush=True)
        save_report(report_path, report)
        if match_stats["verified_image_pairs"] == 0:
            message = "Нет ни одной геометрически подтверждённой пары фотографий."
            report["reconstruction_quality"] = failed_reconstruction_quality(
                report["input_images"], match_stats, message
            )
            save_report(report_path, report)
            raise ReconstructionError(
                "Не найдено геометрически подтверждённых пересечений между фотографиями. "
                "Проверьте перекрытие кадров, резкость и наличие фактуры; построение камер и плотное облако остановлены."
            )
        print("3/4. Построение камер и разреженного облака...", flush=True)
        # COLMAP must read the same working copies that were registered in the
        # database. For CR2 inputs these live under prepared_images, not the
        # original source directory.
        models = pycolmap.incremental_mapping(db, processing_images, sparse, options=mapping,
                                              cancellation_token=watch.token)
        check_cancel(cancel_file)
        if not models:
            message = "Подтверждённые пары найдены, но COLMAP не смог собрать из них модель камер."
            report["reconstruction_quality"] = failed_reconstruction_quality(
                report["input_images"], match_stats, message
            )
            save_report(report_path, report)
            raise ReconstructionError(
                "COLMAP не построил модель камер при наличии некоторых подтверждённых пар. "
                "Возможны слабый параллакс, смаз, повторяющиеся узоры или слишком малое перекрытие."
            )
        model_id, model = max(models.items(), key=lambda item: (item[1].num_reg_images(), item[1].num_points3D()))
        image_mapping = report.get("image_mapping", [])
        all_model_image_names = {
            source_image_name(component.image(image_id).name, image_mapping)
            for component in models.values()
            for image_id in component.reg_image_ids()
        }
        quality, camera_centers = assess_reconstruction_quality(
            model, report["input_images"], match_stats, len(models), image_mapping, all_model_image_names
        )
        write_camera_centers(output / "camera_centers.csv", camera_centers)
        report.update(registered_images=model.num_reg_images(), sparse_points=int(model.num_points3D()),
                      models=len(models), selected_model=int(model_id),
                      reconstruction_quality=quality, camera_centers="camera_centers.csv")
        print(f"Проверка модели: {quality['registered_images']}/{quality['input_images']} кадров, "
              f"ошибка репроекции {quality['mean_reprojection_error_px']} px, "
              f"средний трек {quality['mean_track_length']} наблюдения; "
              f"оценка: {quality['assessment']}.", flush=True)
        for message in quality["messages"]:
            print(f"Проверка качества: {message}", flush=True)
        save_report(report_path, report)
        if len(models) > 1:
            print(f"Предупреждение: найдено моделей: {len(models)}; выбрана модель с большим числом камер.", flush=True)
        if model.num_points3D() < 1:
            raise ReconstructionError(
                "COLMAP зарегистрировал камеры, но не построил ни одной 3D-точки. "
                "Проверьте перекрытие, параллакс и повторяющиеся детали; диагностика сохранена."
            )
        sparse_ply = output / "sparse.ply"
        model.export_PLY(sparse_ply)
        report["sparse_points"] = ply_vertex_count(sparse_ply)
        save_report(report_path, report)
        result_path = sparse_ply
        if mode != "sparse-cpu":
            if not quality["dense_reconstruction_allowed"]:
                raise ReconstructionError(
                    "Плотная реконструкция остановлена проверкой качества. Разреженный файл sparse.ply "
                    "и диагностика сохранены; исправьте съёмку/состав кадров и запустите обработку снова."
                )
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
                report["compute_backends"]["dense_stereo"] = "OpenMVS CPU"
                result_path = output / "dense.ply"
            elif mode == "dense-opencl":
                print("Плотная реконструкция на выбранной OpenCL GPU (эксперимент)…", flush=True)
                run_cl([str(colmap), "patch_match_stereo", "--workspace_path", str(workspace),
                        "--workspace_format", "COLMAP", "--PatchMatchStereo.max_image_size", "1280",
                        "--PatchMatchStereo.num_iterations", "3", "--PatchMatchStereo.geom_consistency", "0",
                        "--PatchMatchStereo.gpu_index", str(selected_gpu["colmap_gpu_index"])], selected_gpu)
                report["compute_backends"]["dense_stereo"] = "COLMAP-CL OpenCL (verified)"
                report["opencl_stages"]["patch_match_stereo"] = {
                    "status": "verified", "backend": "COLMAP-CL OpenCL", "device": selected_gpu["device"],
                }
                report["opencl_device_verified"] = True
                save_report(report_path, report)
                result_path = output / "dense.ply"
                run_cl([str(colmap), "stereo_fusion", "--workspace_path", str(workspace),
                        "--workspace_format", "COLMAP", "--input_type", "photometric",
                        "--output_path", str(result_path)])
                report["compute_backends"]["stereo_fusion"] = "COLMAP CPU"
            else:
                print("Режим AMD требует рабочей HIP/ROCm-сборки COLMAP; совместимость карты не проверена.", flush=True)
                run_program([str(colmap), "patch_match_stereo", "--workspace_path", str(workspace),
                             "--workspace_format", "COLMAP", "--PatchMatchStereo.max_image_size", "1600"], output, cancel_file)
                report["compute_backends"]["dense_stereo"] = "COLMAP HIP requested; GPU not verified"
                result_path = output / "dense.ply"
                run_program([str(colmap), "stereo_fusion", "--workspace_path", str(workspace),
                             "--workspace_format", "COLMAP", "--input_type", "geometric",
                             "--output_path", str(result_path)], output, cancel_file)
                report["compute_backends"]["stereo_fusion"] = "COLMAP CPU"
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
    parser.add_argument("--threads", type=int, default=recommended_threads(),
                        help=f"Потоки CPU (по умолчанию: {recommended_threads()}, максимум: {MAX_THREADS})")
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
