#!/usr/bin/env python3
"""Import documented A2 mapper exports; never execute or inspect its binaries.

This is a research capture bundle, not a Core camera trajectory or .spx writer.
Source snapshots are authoritative. Display PLY is a disposable working copy.
"""
from __future__ import annotations

import argparse
import csv
from decimal import Decimal, InvalidOperation, localcontext
import hashlib
import html
import json
import math
import os
from pathlib import Path
import shutil
import sys
import uuid

VERSION = "1.0.0"
UPSTREAM = "https://github.com/Werdna0107/A2-slam/tree/5212867f9fb7198d55f0a08f72aa0f381c11fa67"
CHUNK = 100_000
MAX_TEXT_BYTES = 128 * 1024 * 1024


class ImportErrorA2(ValueError):
    pass


class Cancelled(Exception):
    pass


def check_cancel(path: Path | None) -> None:
    if path is not None and path.exists():
        raise Cancelled("Импорт остановлен")


def timestamp_ns(value: str, *, nanoseconds: bool = False) -> int:
    try:
        # Decimal preserves epoch nanoseconds; binary floats must never see time.
        if len(value) > 100:
            raise ValueError()
        with localcontext() as context:
            context.prec = 120
            result = Decimal(value) * (1 if nanoseconds else 1_000_000_000)
            if not result.is_finite() or result < 0 or result > 2**63 - 1 or result != result.to_integral_value():
                raise ValueError()
            return int(result)
    except (InvalidOperation, ValueError, OverflowError) as exc:
        raise ImportErrorA2(f"Некорректная временная метка: {value[:100]}") from exc


def finite(value: str) -> float:
    try:
        result = float(value)
        if not math.isfinite(result):
            raise ValueError()
        return result
    except (ValueError, OverflowError) as exc:
        raise ImportErrorA2(f"Некорректное число: {str(value)[:100]}") from exc


def write_json(path: Path, data: dict) -> None:
    with path.open("x", encoding="utf-8") as stream:
        json.dump(data, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def hash_file(path: Path, cancel: Path | None = None) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            check_cancel(cancel)
            block = stream.read(1024 * 1024)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def discover(folder: Path, controls: Path | None = None) -> dict:
    if not folder.is_dir():
        raise ImportErrorA2("Выберите папку результатов A2 mapper с cloud.las и trajectory.txt.")
    files = {p.name.casefold(): p for p in folder.iterdir() if p.is_file()}
    if len(files) != sum(p.is_file() for p in folder.iterdir()):
        raise ImportErrorA2("Имена файлов различаются только регистром; выберите однозначный набор.")
    if "cloud.las" not in files or "trajectory.txt" not in files:
        raise ImportErrorA2("Нужны cloud.las и trajectory.txt. Сначала обработайте .bag/.bin в A2 mapper.")
    if controls is not None:
        if not controls.is_file() or controls.suffix.lower() != ".csv":
            raise ImportErrorA2("Файл опорных точек CSV не найден.")
    else:
        controls = files.get("ctrl_points_edit.csv") or files.get("control_points.csv")
        if controls is None:
            candidates = [p for name, p in files.items() if name.endswith(".gcp.csv")]
            if len(candidates) > 1:
                raise ImportErrorA2("Найдено несколько .gcp.csv; выберите нужный файл явно.")
            controls = candidates[0] if candidates else None
    return {"cloud": files["cloud.las"], "trajectory": files["trajectory.txt"], "controls": controls}


def snapshot(source: Path, destination: Path, cancel: Path | None) -> dict:
    digest = hashlib.sha256()
    with source.open("rb") as incoming, destination.open("xb") as outgoing:
        before = os.fstat(incoming.fileno())
        while True:
            check_cancel(cancel)
            block = incoming.read(1024 * 1024)
            if not block:
                break
            outgoing.write(block)
            digest.update(block)
        after = os.fstat(incoming.fileno())
    identity = lambda s: (s.st_size, s.st_mtime_ns, s.st_ino, s.st_dev)
    if identity(before) != identity(after) or identity(after) != identity(source.stat()):
        raise ImportErrorA2(f"Исходный файл изменился во время копирования: {source.name}")
    return {"original_name": source.name, "sha256": digest.hexdigest(), "bytes": after.st_size,
            "path": "sources/" + destination.name}


def parse_trajectory(source: Path, target: Path, cancel: Path | None = None) -> dict:
    if source.stat().st_size > MAX_TEXT_BYTES:
        raise ImportErrorA2("Траектория превышает поддерживаемый размер 128 МиБ.")
    first = previous = None
    count = normalized = 0
    distance = 0.0
    samples = []
    with source.open(encoding="utf-8-sig") as incoming, target.open("x", encoding="utf-8") as outgoing:
        for line_number, line in enumerate(incoming, 1):
            check_cancel(cancel)
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) != 11:
                raise ImportErrorA2(f"Траектория, строка {line_number}: ожидается 11 полей.")
            values = [finite(x) for x in fields[:10]]
            time = timestamp_ns(fields[10])
            norm = math.hypot(*values[6:10])
            if abs(norm - 1) > .001:
                raise ImportErrorA2(f"Траектория, строка {line_number}: quaternion не единичный.")
            if previous is not None and time <= previous["timestamp_ns"]:
                raise ImportErrorA2(f"Траектория, строка {line_number}: время не возрастает.")
            pose = {"source_line": line_number, "timestamp_ns": time, "position_m": values[:3],
                    "rpy_rad_source": values[3:6], "quaternion_xyzw": [x / norm for x in values[6:10]]}
            normalized += int(abs(norm - 1) > 1e-12)
            if first is None:
                first = pose
            if previous:
                distance += math.dist(pose["position_m"], previous["position_m"])
                if not math.isfinite(distance):
                    raise ImportErrorA2("Траектория выходит за числовой диапазон.")
            outgoing.write(json.dumps(pose, allow_nan=False) + "\n")
            previous = pose
            count += 1
            # Bounded route sample for the report; the JSONL retains every pose.
            if len(samples) < 2000:
                samples.append(pose["position_m"])
    if count < 2:
        raise ImportErrorA2("В траектории должно быть минимум две позы.")
    return {"pose_count": count, "start_ns": first["timestamp_ns"], "end_ns": previous["timestamp_ns"],
            "distance_m": distance, "endpoint_gap_m": math.dist(first["position_m"], previous["position_m"]),
            "normalized_quaternions": normalized, "path": "trajectory.jsonl",
            "clock_domain": "scanner_unverified", "pose_frame": "scanner_reference_unverified",
            "transform_direction": "unverified", "orientation_rpy_convention": "unverified",
            "loop_closure_status": "not_assessed", "route_preview_first_poses": samples}


def parse_controls(source: Path, trajectory: dict, cancel: Path | None = None) -> dict:
    if source.stat().st_size > MAX_TEXT_BYTES:
        raise ImportErrorA2("CSV превышает поддерживаемый размер 128 МиБ.")
    accepted, rejected = [], []
    with source.open(encoding="utf-8-sig", newline="") as incoming:
        reader = csv.DictReader(incoming)
        names = [(name or "").strip().casefold() for name in reader.fieldnames or []]
        if len(set(names)) != len(names):
            raise ImportErrorA2("В CSV повторяются названия столбцов.")
        share = {"timestamp", "x", "y", "z"}.issubset(names)
        a2 = {"board_real_ns", "lat_deg", "lon_deg", "h_ellps_m"}.issubset(names)
        if share == a2:
            raise ImportErrorA2("Нужен CSV SHARE (timestamp,x,y,z) или A2 (board_real_ns,lat_deg,lon_deg,h_ellps_m).")
        for row in reader:
            check_cancel(cancel)
            row_number = reader.line_num
            values = {(key or "").strip().casefold(): (value or "").strip()
                      for key, value in row.items() if key is not None}
            try:
                if None in row:
                    raise ImportErrorA2("Лишние значения в строке CSV")
                name = values.get("name") or f"row-{row_number}"
                if share:
                    time = timestamp_ns(values["timestamp"])
                    point = {"position_m": [finite(values[k]) for k in ("x", "y", "z")],
                             "coordinate_frame": "control_coordinates_unverified"}
                else:
                    # Missing solution status must not silently become RTK fixed.
                    if values.get("carr_soln") != "2":
                        raise ImportErrorA2("RTK fixed не подтверждён (нужен carr_soln=2)")
                    time = timestamp_ns(values["board_real_ns"], nanoseconds=True)
                    lat, lon, altitude = [finite(values[k]) for k in ("lat_deg", "lon_deg", "h_ellps_m")]
                    if not -90 <= lat <= 90 or not -180 <= lon <= 180:
                        raise ImportErrorA2("Широта/долгота вне диапазона")
                    point = {"latitude_deg": lat, "longitude_deg": lon, "ellipsoidal_height_m": altitude,
                             "coordinate_frame": "geodetic_datum_unverified", "rtk_fixed": True}
                    for key in ("hacc_m", "vacc_m"):
                        if values.get(key):
                            point[key] = finite(values[key])
                            if point[key] < 0:
                                raise ImportErrorA2("Точность не может быть отрицательной")
                in_range = trajectory["start_ns"] <= time <= trajectory["end_ns"]
                accepted.append({"source_line": row_number, "name": name, "timestamp_ns": time,
                                 "within_trajectory_time_range": in_range, "clock_alignment": "unverified",
                                 "role": "unassigned", "used_for_correction": False, **point})
            except ImportErrorA2 as exc:
                rejected.append({"source_line": row_number, "reason": str(exc)})
    return {"format": "share" if share else "a2_rtk", "accepted": accepted, "rejected": rejected,
            "accepted_count": len(accepted), "rejected_count": len(rejected),
            "correction_applied": False, "checkpoint_rmse_m": None}


def convert_cloud(source: Path, target: Path, cancel: Path | None = None,
                  rgb_encoding: str = "uint16", up_axis: str = "z") -> dict:
    import laspy
    import numpy as np

    if rgb_encoding not in ("uint16", "uint8") or up_axis not in ("y", "z"):
        raise ImportErrorA2("Неподдерживаемые параметры цвета или вертикальной оси.")
    with laspy.open(source) as reader:
        header = reader.header
        if header.are_points_compressed:
            raise ImportErrorA2("Сжатый LAZ не поддерживается; экспортируйте несжатый LAS.")
        if not header.point_count:
            raise ImportErrorA2("LAS не содержит точек.")
        if header.offset_to_point_data + header.point_count * header.point_format.size > source.stat().st_size:
            raise ImportErrorA2("LAS обрезан: данных меньше, чем указано в заголовке.")
        if not np.isfinite(header.scales).all() or not (header.scales > 0).all() or not np.isfinite(header.offsets).all():
            raise ImportErrorA2("Некорректные масштабы или смещения LAS.")
        colored = {"red", "green", "blue"}.issubset(header.point_format.dimension_names)
        dtype = np.dtype([("x", "<f8"), ("y", "<f8"), ("z", "<f8"),
                          ("red", "u1"), ("green", "u1"), ("blue", "u1")])
        minimum, maximum = np.full(3, np.inf), np.full(3, -np.inf)
        count = 0
        with target.open("xb") as outgoing:
            outgoing.write(("ply\nformat binary_little_endian 1.0\n"
                            "comment SpatialPhotoCloud units meters\n"
                            f"comment SpatialPhotoCloud up {up_axis}\n"
                            "comment A2 source units declared; accuracy unverified\n"
                            f"element vertex {header.point_count}\n"
                            "property double x\nproperty double y\nproperty double z\n"
                            "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n").encode("ascii"))
            for chunk in reader.chunk_iterator(CHUNK):
                check_cancel(cancel)
                converted = np.empty(len(chunk), dtype=dtype)
                for axis, key in enumerate(("x", "y", "z")):
                    coordinates = np.asarray(getattr(chunk, key))
                    if not np.isfinite(coordinates).all():
                        raise ImportErrorA2("LAS содержит некорректные координаты.")
                    converted[key] = coordinates
                    minimum[axis] = min(minimum[axis], coordinates.min())
                    maximum[axis] = max(maximum[axis], coordinates.max())
                for key in ("red", "green", "blue"):
                    if colored:
                        color = np.asarray(getattr(chunk, key))
                        if rgb_encoding == "uint8" and (color > 255).any():
                            raise ImportErrorA2("В LAS есть RGB выше 255; выберите uint16.")
                        converted[key] = color if rgb_encoding == "uint8" else ((color.astype("u4") + 128) // 257)
                    else:
                        converted[key] = 180
                outgoing.write(converted.tobytes())
                count += len(chunk)
        if count != header.point_count:
            raise ImportErrorA2("Количество прочитанных точек не совпадает с LAS.")
        crs_records = [{"user_id": v.user_id, "record_id": v.record_id}
                       for v in header.vlrs if v.user_id == "LASF_Projection"]
        return {"point_count": count, "colored": colored, "rgb_encoding": rgb_encoding,
                "version": str(header.version), "point_format": header.point_format.id,
                "scales": header.scales.tolist(), "offsets": header.offsets.tolist(),
                "bounds_min": minimum.tolist(), "bounds_max": maximum.tolist(),
                "projection_vlrs": crs_records, "crs_interpretation": "not_applied",
                "units": "meters_declared_from_a2_export", "up_axis_display": up_axis,
                "display_path": "../cloud.ply", "display_coordinates": "unchanged_float64",
                "source_attributes": "all retained in sources/cloud.las"}


def report_html(report: dict, controls: dict) -> str:
    trajectory = report["trajectory"]
    route = trajectory["route_preview_first_poses"]
    xs, ys = [p[0] for p in route], [p[1] for p in route]
    scale = max(max(xs) - min(xs), max(ys) - min(ys), 1e-9)
    points = " ".join(f"{20 + (x - min(xs))/scale*660:.2f},{360 - (y-min(ys))/scale*330:.2f}"
                      for x, y in zip(xs, ys))
    rows = "".join(f"<tr><td>{html.escape(p['name'])}</td><td>{p['timestamp_ns']}</td>"
                   f"<td>{'В диапазоне' if p['within_trajectory_time_range'] else 'Вне диапазона'}</td></tr>"
                   for p in controls["accepted"])
    rejects = "".join(f"<li>Строка {p['source_line']}: {html.escape(p['reason'])}</li>" for p in controls["rejected"])
    return f"""<!doctype html><html lang="ru"><meta charset="utf-8"><title>Импорт съёмки A2</title>
<style>body{{font:16px system-ui;max-width:900px;margin:32px auto;padding:0 20px;color:#243041}}table{{border-collapse:collapse;width:100%}}td,th{{padding:10px;border-bottom:1px solid #ddd;text-align:left}}svg{{width:100%;background:#eef2f7}}code{{overflow-wrap:anywhere}}.note{{background:#fff2d2;padding:14px}}</style>
<h1>Импорт съёмки A2</h1><p>Сессия: <code>{report['session_id']}</code></p>
<p>{report['cloud']['point_count']:,} точек · {trajectory['pose_count']:,} поз · путь {trajectory['distance_m']:.3f} м</p>
<p class="note">Координаты сохранены без преобразования. Точность, система координат, привязка времени и смысл ориентации не подтверждены. Коррекция по опорам не выполнялась.</p>
<h2>Проекция траектории на исходную плоскость XY</h2>
<p>Первые {len(route)} поз; полный маршрут сохранён в trajectory.jsonl. Вертикальная ось здесь не определяется автоматически.</p>
<svg viewBox="0 0 700 380" role="img" aria-label="Проекция траектории"><polyline points="{points}" fill="none" stroke="#1763d5" stroke-width="2"/></svg>
<p>Расстояние между началом и концом: {trajectory['endpoint_gap_m']:.3f} м. Это не оценка точности и не подтверждение замыкания петли.</p>
<h2>Опорные точки</h2><p>Принято: {controls['accepted_count']}; отклонено: {controls['rejected_count']}. Совпадение диапазона времени не доказывает синхронизацию.</p>
<table><tr><th>Имя</th><th>Время, нс</th><th>Диапазон траектории</th></tr>{rows}</table><ul>{rejects}</ul>
<p>Исходные LAS, TXT и выбранный CSV сохранены в sources/. Параметры и SHA-256 находятся в manifest.json. Облако cloud.ply — рабочая копия для просмотра.</p></html>"""


def import_capture(source: Path, output: Path, *, controls: Path | None = None,
                   coordinate_frame: str = "a2_export_local_unverified", source_version: str = "unknown",
                   rgb_encoding: str = "uint16", up_axis: str = "z", cancel: Path | None = None,
                   log=print) -> dict:
    source, output = source.resolve(), output.resolve()
    if not coordinate_frame.strip() or len(coordinate_frame) > 200 or any(ord(c) < 32 for c in coordinate_frame):
        raise ImportErrorA2("Укажите корректное имя системы координат (до 200 символов).")
    selected = discover(source, controls)
    check_cancel(cancel)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive reservation prevents overwrite; manifest is the commit marker.
    output.mkdir(exist_ok=False)
    try:
        data = output / "capture"
        data.mkdir()
        sources = data / "sources"
        sources.mkdir()
        source_records = []
        for kind, filename in (("cloud", "cloud.las"), ("trajectory", "trajectory.txt"), ("controls", "control_points.csv")):
            path = selected[kind]
            if path is not None:
                log(f"Сохранение исходника: {path.name}")
                source_records.append({"role": kind, **snapshot(path, sources / filename, cancel)})
        log("Проверка траектории…")
        trajectory = parse_trajectory(sources / "trajectory.txt", data / "trajectory.jsonl", cancel)
        control_data = parse_controls(sources / "control_points.csv", trajectory, cancel) if selected["controls"] else {
            "format": None, "accepted": [], "rejected": [], "accepted_count": 0, "rejected_count": 0,
            "correction_applied": False, "checkpoint_rmse_m": None}
        write_json(data / "control_points.json", control_data)
        log("Преобразование LAS в PLY без прореживания…")
        cloud = convert_cloud(sources / "cloud.las", output / "cloud.ply", cancel, rgb_encoding, up_axis)
        configuration = {"coordinate_frame": coordinate_frame, "rgb_encoding": rgb_encoding,
                         "up_axis_display": up_axis, "source_version": source_version,
                         "importer_version": VERSION, "time_domain": "scanner_unverified",
                         "source_units": "meters_declared_from_a2_export"}
        write_json(data / "configuration.json", configuration)
        artifacts = [{"path": name, "sha256": hash_file(data / name, cancel)}
                     for name in ("trajectory.jsonl", "control_points.json", "configuration.json")]
        report = {"format": "spatial-research-capture", "format_version": 1, "status": "imported_unverified",
                  "session_id": str(uuid.uuid4()), "coordinate_frame": coordinate_frame,
                  "producer": {"name": "spatial-a2-import", "version": VERSION},
                  "source_producer": {"name": "A2 mapper", "version": source_version, "identity_verified": False},
                  "format_reference": UPSTREAM, "sources": source_records,
                  "parameters": configuration, "artifacts": artifacts,
                  "configuration_sha256": artifacts[-1]["sha256"],
                  "working_cloud": {"path": "../cloud.ply", "mutable": True,
                                    "authoritative_source": "sources/cloud.las"},
                  "trajectory": trajectory, "cloud": cloud,
                  "control_points": {k: v for k, v in control_data.items() if k not in ("accepted", "rejected")},
                  "validation": {"metric_accuracy": "unverified", "crs": "unverified", "clock_sync": "unverified",
                                 "camera_pose_compatible": False, "core_imported": False,
                                 "trajectory_correction_applied": False, "raw_scan_processing": False}}
        (data / "report.html").write_text(report_html(report, control_data), encoding="utf-8")
        check_cancel(cancel)
        # Readers must never see a partly serialized commit marker.
        temporary_manifest = data / ".manifest.json.pending"
        write_json(temporary_manifest, report)
        temporary_manifest.rename(data / "manifest.json")
        log(f"Готово: {cloud['point_count']} точек, {trajectory['pose_count']} поз. Отчёт: capture/report.html")
        return report
    except BaseException:
        shutil.rmtree(output)
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Импорт папки результатов A2 mapper (LAS + trajectory.txt)")
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="Новая папка результата")
    parser.add_argument("--controls", type=Path)
    parser.add_argument("--coordinate-frame", default="a2_export_local_unverified")
    parser.add_argument("--source-version", default="unknown")
    parser.add_argument("--rgb-encoding", choices=("uint16", "uint8"), default="uint16")
    parser.add_argument("--up-axis", choices=("y", "z"), default="z")
    parser.add_argument("--cancel-file", type=Path)
    args = parser.parse_args(argv)
    try:
        import_capture(args.input, args.output, controls=args.controls, coordinate_frame=args.coordinate_frame,
                       source_version=args.source_version, rgb_encoding=args.rgb_encoding, up_axis=args.up_axis,
                       cancel=args.cancel_file, log=lambda text: print(text, flush=True))
        return 0
    except Cancelled:
        print("Импорт остановлен; исходники не изменены.", flush=True)
        return 130
    except Exception as exc:
        print(f"Ошибка импорта A2: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
