"""Read a verified research capture for joint, read-only viewing."""
import json
import math
from pathlib import Path
import re

from a2_import import hash_file


def open_capture(folder: Path) -> tuple[Path, dict]:
    root = folder.resolve(strict=True)
    if root.name == "capture":
        root = root.parent

    def local(name):
        path = root / name
        if any(p.is_symlink() for p in (path, *path.parents) if p.is_relative_to(root)):
            raise ValueError("Ссылки на внешние файлы в съёмке не поддерживаются.")
        if not path.is_file() or not path.resolve().is_relative_to(root):
            raise ValueError(f"Файл съёмки отсутствует: {name}")
        return path

    def read_json(name):
        path = local(name)
        if path.stat().st_size > 128 * 1024 * 1024:
            raise ValueError("Файл метаданных слишком велик.")
        return json.loads(path.read_text(encoding="utf-8"))

    manifest = read_json("capture/manifest.json")
    if manifest.get("format") != "spatial-research-capture" or manifest.get("format_version") != 1:
        raise ValueError("Неподдерживаемый формат съёмки.")
    expected = {item["path"]: item["sha256"] for item in manifest.get("artifacts", [])}
    for name in ("trajectory.jsonl", "control_points.json", "configuration.json"):
        digest = expected.get(name, "")
        if not re.fullmatch(r"[0-9a-f]{64}", digest) or hash_file(local("capture/" + name)) != digest:
            raise ValueError(f"Контрольная сумма не совпадает: {name}")
    cloud = local("cloud.ply")
    digest = manifest.get("working_cloud", {}).get("sha256_at_import")
    if not digest:
        raise ValueError("В старой съёмке нет контрольной суммы облака. Повторите импорт для совместного просмотра.")
    if hash_file(cloud) != digest:
        raise ValueError("Рабочий PLY изменён: траектория больше не гарантированно совпадает с облаком. Повторите импорт или откройте PLY отдельно.")
    config = read_json("capture/configuration.json")
    if config.get("up_axis_display") not in ("y", "z"):
        raise ValueError("Неизвестная вертикальная ось.")
    route = []
    previous = -1
    trajectory = local("capture/trajectory.jsonl")
    if trajectory.stat().st_size > 512 * 1024 * 1024:
        raise ValueError("Траектория слишком велика для просмотра.")
    def xyz(value):
        if not isinstance(value, list) or len(value) != 3 or any(type(v) not in (int, float) or not math.isfinite(v) for v in value):
            raise ValueError("Некорректные координаты в съёмке.")
        return value
    total = manifest["trajectory"]["pose_count"]
    if type(total) is not int or total < 2:
        raise ValueError("Некорректное число поз.")
    stride = max(1, math.ceil((total - 1) / 1999))
    count = 0
    with trajectory.open(encoding="utf-8") as stream:
        for count, line in enumerate(stream, 1):
            pose = json.loads(line)
            stamp = pose["timestamp_ns"]
            if type(stamp) is not int or not 0 <= stamp <= 2**63 - 1 or stamp <= previous:
                raise ValueError("Неупорядоченное время траектории.")
            previous = stamp
            position = xyz(pose["position_m"])
            if (count - 1) % stride == 0 or count == total:
                route.append({"position": position, "timestamp_ns": str(stamp), "index": count - 1})
    if count != total:
        raise ValueError("Число поз не совпадает с manifest.")
    controls = read_json("capture/control_points.json")
    marks = [{"name": str(p["name"]), "position": xyz(p["position_m"])}
             for p in controls["accepted"] if "position_m" in p]
    return cloud, {"session_id": manifest["session_id"], "coordinate_frame": manifest["coordinate_frame"],
                   "route": route, "pose_count": total, "controls": marks[:2000],
                   "control_count": len(marks), "geodetic_controls": len(controls["accepted"]) - len(marks),
                   "read_only": True, "accuracy": "unverified"}
