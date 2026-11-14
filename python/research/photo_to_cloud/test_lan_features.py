"""Checks for the same project running locally or on two LAN processes."""

from __future__ import annotations

from contextlib import closing
import json
import os
from pathlib import Path
from queue import Empty, Queue
import re
import sqlite3
import subprocess
import sys
from tempfile import TemporaryDirectory
from threading import Thread
import time
import unittest

from lan_features import PAIRING_ENV, NodeError, import_features


def wait_for_node_line(lines: Queue, match):
    deadline = time.monotonic() + 30
    while True:
        try:
            line = lines.get(timeout=max(0, deadline - time.monotonic()))
        except Empty as exc:
            raise AssertionError("Timed out waiting for the LAN node") from exc
        if line is None:
            raise AssertionError("LAN node stopped before the expected message")
        result = match(line.strip())
        if result:
            return result


def terminate(process: subprocess.Popen, force: bool = False) -> None:
    if process.poll() is None:
        process.kill() if force else process.terminate()
    process.wait(timeout=15)


def run_project(command: list[str], images: Path, root: Path, label: str) -> tuple[dict, Path]:
    return collect_project(start_project(command, images, root, label), root, label)


def start_project(command: list[str], images: Path, root: Path, label: str, code: str = "") -> subprocess.Popen:
    environment = os.environ.copy()
    environment["PYTHONUTF8"] = "1"
    environment.pop(PAIRING_ENV, None)
    if code:
        environment[PAIRING_ENV] = code
    with (root / f"{label}.log").open("wb") as log:
        return subprocess.Popen(
            command + ["--worker", "--images", str(images), "--output", str(root / label), "--threads", "2"],
            env=environment, stdout=log, stderr=subprocess.STDOUT,
        )


def collect_project(process: subprocess.Popen, root: Path, label: str) -> tuple[dict, Path]:
    try:
        if process.wait(timeout=90) != 0:
            raise AssertionError((root / f"{label}.log").read_text(encoding="utf-8", errors="replace"))
    finally:
        terminate(process)
    output = root / label
    return json.loads((output / "report.json").read_text(encoding="utf-8")), output / "database.db"


def feature_rows(database: Path) -> list:
    with closing(sqlite3.connect(database)) as db:
        return db.execute(
            """SELECT name, model, width, height, params, prior_focal_length, k.rows, k.data, d.type, d.data
               FROM images JOIN cameras USING (camera_id) JOIN keypoints k USING (image_id)
               JOIN descriptors d USING (image_id) ORDER BY name"""
        ).fetchall()


def run_with_node(command: list[str], images: Path, root: Path, label: str, scenario: str) -> tuple[dict, Path]:
    with (root / f"{label}-node.log").open("wb") as log, subprocess.Popen(
        command + ["--lan-node", "--host", "127.0.0.1", "--port", "0", "--threads", "2"],
        stdout=subprocess.PIPE, stderr=log, text=True, encoding="utf-8", errors="replace",
    ) as node:
        lines: Queue = Queue()
        project = None

        def read_lines() -> None:
            try:
                for line in node.stdout:
                    lines.put(line)
            finally:
                lines.put(None)

        reader = Thread(target=read_lines, daemon=True)
        reader.start()
        try:
            code = wait_for_node_line(lines, lambda line: re.fullmatch(
                r"127\.0\.0\.1:\d+/[0-9a-f]{64}\.[0-9a-f]{32}", line
            )).group()
            if scenario == "wrong_certificate":
                host, credentials = code.split("/")
                replacement = "1" if credentials.startswith("0") else "0"
                code = f"{host}/{replacement}{credentials[1:]}"
            project = start_project(command, images, root, label, code)
            if scenario == "disconnect":
                wait_for_node_line(lines, lambda line: line.startswith("Получен пакет:"))
                terminate(node, force=True)
            return collect_project(project, root, label)
        finally:
            if project is not None:
                terminate(project)
            terminate(node)
            reader.join(timeout=3)


def verify_lan_reconstruction(command: list[str], images: Path, root: Path) -> None:
    baseline, baseline_db = run_project(command, images, root, "local")
    # Compare by original name, including camera calibration and raw SIFT matrices.
    expected_rows = feature_rows(baseline_db)
    if len(expected_rows) != 7 or baseline["registered_images"] < 3:
        raise AssertionError(
            f"Invalid reference: SIFT for {[row[0] for row in expected_rows]}, "
            f"{baseline['registered_images']} registered cameras"
        )
    for scenario in ("distributed", "disconnect", "wrong_certificate"):
        report, database = run_with_node(command, images, root, scenario, scenario)
        stats = report["lan_feature_extraction"]
        if scenario == "distributed":
            assert stats["status"] == "distributed" and stats["remote_images"] > 0 and stats["local_images"] > 0, stats
        elif scenario == "disconnect":
            assert stats["fallback_batches"] == 1 and stats["local_images"] == 7 and stats["remote_images"] == 0, stats
        else:
            assert stats["status"] == "local_fallback" and stats["local_images"] == 7, stats
            assert "Отпечаток сертификата" in (root / f"{scenario}.log").read_text(encoding="utf-8"), stats
        assert feature_rows(database) == expected_rows, scenario
        assert report["registered_images"] == baseline["registered_images"], report
        assert report["verified_matches"]["verified_image_pairs"] == baseline["verified_matches"]["verified_image_pairs"]
        assert report["point_count"] > 0 and (database.parent / "sparse.ply").is_file(), report
        print(f"LAN {scenario}: {stats}, {report['registered_images']} cameras, {report['point_count']} points")


class LanFeaturesTests(unittest.TestCase):
    def test_network_flows_and_local_reference(self):
        from generate_test_images import generate

        with TemporaryDirectory() as directory:
            root = Path(directory)
            images = root / "images"
            generate(images)
            # Ensure that wire filenames never change relative paths in the project.
            nested = images / "nested photos"
            nested.mkdir()
            (images / "004.png").rename(nested / "004.png")
            verify_lan_reconstruction([sys.executable, str(Path(__file__).with_name("app.py"))], images, root)

    def test_invalid_packet_rolls_back_import(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            database = root / "database.db"
            packet = root / "truncated.sift"
            packet.write_bytes(bytes(4))
            with closing(sqlite3.connect(database)) as db, db:
                db.executescript(
                    """CREATE TABLE images (image_id INTEGER, name TEXT);
                       INSERT INTO images VALUES (1, 'x.png'), (2, 'y.png');
                       CREATE TABLE keypoints (image_id, rows, cols, data);
                       CREATE TABLE descriptors (image_id, type, rows, cols, data);"""
                )
            with self.assertRaises(NodeError):
                import_features(database, ["x.png", "y.png"], packet, lambda: None)
            with closing(sqlite3.connect(database)) as db:
                self.assertEqual(db.execute("SELECT count(*) FROM keypoints").fetchone()[0], 0)
                self.assertEqual(db.execute("SELECT count(*) FROM descriptors").fetchone()[0], 0)
