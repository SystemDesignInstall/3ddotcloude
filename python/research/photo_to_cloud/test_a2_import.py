"""Synthetic independent exports exercise import, precision and publication failures."""
import hashlib
import json
import shutil
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

import laspy
import numpy as np

import a2_import as a2
from app import App, a2_worker_command


class CaptureImportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.output = self.root / "result"
        self.make_las()
        (self.source / "trajectory.txt").write_text(
            "# A2 documented 11-field export\n"
            "2170499.478 312016.336 240.424 0 0 0 0 0 0 1 1735725777.104061985\n"
            "2170502.478 312020.336 240.424 0 0 0 0 0 0 1 1735725778.104061985\n", encoding="utf-8")

    def make_las(self, point_format=3, version="1.2"):
        header = laspy.LasHeader(point_format=point_format, version=version)
        header.scales = np.array([.001, .001, .001])
        header.offsets = np.array([2170499., 312016., 240.])
        cloud = laspy.LasData(header)
        cloud.x = np.array([2170499.478, 2170499.479, 2170502.478])
        cloud.y = np.array([312016.336, 312016.337, 312020.336])
        cloud.z = np.array([240.424, 240.425, 240.424])
        if "red" in cloud.point_format.dimension_names:
            cloud.red = [65535, 0, 32768]
            cloud.green = [0, 65535, 32768]
            cloud.blue = [0, 0, 65535]
        cloud.write(self.source / "cloud.las")

    def run_import(self, **kwargs):
        return a2.import_capture(self.source, self.output, log=lambda _: None, **kwargs)

    def csv(self, text, name="ctrl_points_edit.csv"):
        path = self.source / name
        path.write_text(text, encoding="utf-8")
        return path

    def test_roundtrip_every_point_preserves_large_coordinates_and_sources(self):
        before = {p.name: p.read_bytes() for p in self.source.iterdir()}
        with patch.object(a2, "CHUNK", 2):
            report = self.run_import()
        self.assertEqual(report["cloud"]["point_count"], 3)
        data = (self.output / "cloud.ply").read_bytes()
        payload = data.split(b"end_header\n", 1)[1]
        points = list(struct.iter_unpack("<dddBBB", payload))
        self.assertEqual(len(points), 3)
        self.assertAlmostEqual(points[0][0], 2170499.478, places=8)
        self.assertAlmostEqual(points[1][0] - points[0][0], .001, places=8)
        self.assertEqual(points[0][3:], (255, 0, 0))
        self.assertEqual(points[2][3:], (128, 128, 255))
        for item in report["sources"]:
            stored = self.output / "capture" / item["path"]
            self.assertEqual(stored.read_bytes(), before[item["original_name"]])
            self.assertEqual(hashlib.sha256(stored.read_bytes()).hexdigest(), item["sha256"])
        self.assertEqual({p.name: p.read_bytes() for p in self.source.iterdir()}, before)
        self.assertFalse(report["validation"]["core_imported"])
        self.assertNotIn(str(self.root), json.dumps(report))
        for item in report["artifacts"]:
            artifact = self.output / "capture" / item["path"]
            self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(), item["sha256"])
        config = self.output / "capture/configuration.json"
        self.assertEqual(hashlib.sha256(config.read_bytes()).hexdigest(), report["configuration_sha256"])

    def test_nanosecond_epoch_and_path_length(self):
        report = self.run_import()
        poses = [json.loads(line) for line in (self.output / "capture/trajectory.jsonl").read_text().splitlines()]
        self.assertEqual(poses[0]["timestamp_ns"], 1735725777104061985)
        self.assertEqual(report["trajectory"]["distance_m"], 5.0)
        self.assertEqual(report["trajectory"]["endpoint_gap_m"], 5.0)
        self.assertEqual(report["trajectory"]["loop_closure_status"], "not_assessed")

    def test_legacy_gray_and_las14_color(self):
        for fmt, version in ((0, "1.2"), (7, "1.4")):
            with self.subTest(fmt=fmt):
                self.make_las(fmt, version)
                target = self.root / f"result-{fmt}"
                result = a2.import_capture(self.source, target, log=lambda _: None)
                self.assertEqual(result["cloud"]["colored"], fmt == 7)
                point = next(struct.iter_unpack("<dddBBB", (target / "cloud.ply").read_bytes().split(b"end_header\n", 1)[1]))
                self.assertEqual(point[3:], (255, 0, 0) if fmt == 7 else (180, 180, 180))

    def test_explicit_8bit_colors(self):
        cloud = laspy.read(self.source / "cloud.las")
        cloud.red = [255, 0, 128]
        cloud.green = [0, 255, 128]
        cloud.blue = [0, 0, 255]
        cloud.write(self.source / "cloud.las")
        self.run_import(rgb_encoding="uint8", up_axis="y")
        data = (self.output / "cloud.ply").read_bytes()
        self.assertIn(b"comment SpatialPhotoCloud up y", data)
        self.assertEqual(next(struct.iter_unpack("<dddBBB", data.split(b"end_header\n", 1)[1]))[3:], (255, 0, 0))

    def test_wrong_8bit_option_does_not_clip_silently(self):
        with self.assertRaisesRegex(a2.ImportErrorA2, "выше 255"):
            self.run_import(rgb_encoding="uint8")
        self.assertFalse(self.output.exists())

    def test_share_csv_keeps_rejected_rows_and_out_of_range_marks(self):
        self.csv("Name,timestamp,x,y,z,extra\n"
                 "<script>,1735725777.104061985,1,2,3,note\n"
                 "missing,1735725777.5,,2,3,note\n"
                 "outside,1735725799,4,5,6,note\n"
                 "bad,1735725777.6,NaN,5,6,note\n")
        self.run_import()
        report = json.loads((self.output / "capture/control_points.json").read_text())
        self.assertEqual(report["accepted_count"], 2)
        self.assertEqual(report["rejected_count"], 2)
        self.assertTrue(report["accepted"][0]["within_trajectory_time_range"])
        self.assertFalse(report["accepted"][1]["within_trajectory_time_range"])
        self.assertEqual(report["rejected"][0]["source_line"], 3)
        self.assertFalse(report["correction_applied"])
        self.assertIsNone(report["checkpoint_rmse_m"])
        page = (self.output / "capture/report.html").read_text()
        self.assertIn("&lt;script&gt;", page)
        self.assertNotIn("<script>", page)

    def test_a2_rtk_requires_fixed_and_preserves_geodetic_coordinates(self):
        self.csv("board_real_ns,lat_deg,lon_deg,h_ellps_m,carr_soln,hacc_m,vacc_m\n"
                 "1735725777104061985,55.5,37.5,200,2,0.02,0.03\n"
                 "1735725777204061985,55.5,37.5,200,1,0.02,0.03\n"
                 "1735725777304061985,95,37.5,200,2,0.02,0.03\n"
                 "1735725777404061985,55.5,37.5,200,2,-1,0.03\n", "scan_00001.gcp.csv")
        self.run_import()
        controls = json.loads((self.output / "capture/control_points.json").read_text())
        self.assertEqual(controls["accepted_count"], 1)
        self.assertEqual(controls["rejected_count"], 3)
        self.assertEqual(controls["accepted"][0]["timestamp_ns"], 1735725777104061985)
        self.assertEqual(controls["accepted"][0]["ellipsoidal_height_m"], 200)
        self.assertNotIn("enu_origin", controls)

    def test_missing_rtk_status_is_not_assumed_fixed(self):
        self.csv("board_real_ns,lat_deg,lon_deg,h_ellps_m\n1,55,37,200\n", "scan_1.gcp.csv")
        report = self.run_import()
        self.assertEqual(report["control_points"]["accepted_count"], 0)
        self.assertEqual(report["control_points"]["rejected_count"], 1)

    def test_controls_precedence_and_external_selection(self):
        self.csv("wrong", "control_points.csv")
        preferred = self.csv("Name,timestamp,x,y,z\np,1735725777.5,1,2,3\n")
        self.assertEqual(a2.discover(self.source)["controls"], preferred)
        external = self.root / "external.csv"
        external.write_text(preferred.read_text())
        report = self.run_import(controls=external)
        self.assertEqual(report["sources"][-1]["original_name"], "external.csv")

    def test_ambiguous_controls_rejected(self):
        self.csv("a", "scan_1.gcp.csv")
        self.csv("b", "scan_2.gcp.csv")
        with self.assertRaisesRegex(a2.ImportErrorA2, "несколько"):
            self.run_import()
        self.assertFalse(self.output.exists())

    def test_duplicate_headers_rejected(self):
        self.csv("Name,timestamp,x,y,z,X\n")
        with self.assertRaisesRegex(a2.ImportErrorA2, "повторяются"):
            self.run_import()
        self.assertFalse(self.output.exists())

    def test_bad_csv_structure_rejected(self):
        self.csv("Name,foo,bar\n")
        with self.assertRaises(a2.ImportErrorA2):
            self.run_import()
        self.assertFalse(self.output.exists())

    def test_existing_destination_never_overwritten(self):
        self.output.mkdir()
        marker = self.output / "keep"
        marker.write_text("important")
        with self.assertRaises(FileExistsError):
            self.run_import()
        self.assertEqual(marker.read_text(), "important")

    def test_truncated_las_is_not_published(self):
        path = self.source / "cloud.las"
        path.write_bytes(path.read_bytes()[:-1])
        with self.assertRaisesRegex(a2.ImportErrorA2, "обрезан"):
            self.run_import()
        self.assertFalse(self.output.exists())

    def test_empty_cloud_rejected(self):
        laspy.LasData(laspy.LasHeader(point_format=3, version="1.2")).write(self.source / "cloud.las")
        with self.assertRaisesRegex(a2.ImportErrorA2, "не содержит"):
            self.run_import()
        self.assertFalse(self.output.exists())

    def test_bad_scale_rejected(self):
        path = self.source / "cloud.las"
        data = bytearray(path.read_bytes())
        struct.pack_into("<d", data, 131, float("nan"))
        path.write_bytes(data)
        with self.assertRaisesRegex(a2.ImportErrorA2, "масштабы"):
            self.run_import()
        self.assertFalse(self.output.exists())

    def test_bad_trajectory_rejected_before_publication(self):
        path = self.source / "trajectory.txt"
        original = path.read_text()
        cases = [original.replace("0 0 0 1", "0 0 0 2"),
                 original.replace("1735725778.104061985", "1735725777.104061985"),
                 original.replace("1735725778.104061985", "1735725778.1040619851"),
                 original.replace("2170499.478", "nan"), "1 2 3\n"]
        for source in cases:
            with self.subTest(source=source):
                path.write_text(source)
                with self.assertRaises(a2.ImportErrorA2):
                    self.run_import()
                self.assertFalse(self.output.exists())

    def test_quaternion_within_tolerance_is_normalized_and_source_retained(self):
        path = self.source / "trajectory.txt"
        path.write_text(path.read_text().replace("0 0 0 1", "0 0 0 1.0005"))
        report = self.run_import()
        self.assertEqual(report["trajectory"]["normalized_quaternions"], 2)
        pose = json.loads((self.output / "capture/trajectory.jsonl").read_text().splitlines()[0])
        self.assertEqual(pose["quaternion_xyzw"], [0, 0, 0, 1])
        self.assertIn("1.0005", (self.output / "capture/sources/trajectory.txt").read_text())

    def test_timestamp_edge_cases(self):
        self.assertEqual(a2.timestamp_ns("1.735725777104061985e9"), 1735725777104061985)
        self.assertEqual(a2.timestamp_ns("0"), 0)
        for value in ("nan", "Infinity", "-1", "1e100", "1e-10", "9223372036.854775808"):
            with self.subTest(value=value), self.assertRaises(a2.ImportErrorA2):
                a2.timestamp_ns(value)

    def test_cancel_before_import_and_during_copy(self):
        cancel = self.root / "cancel"
        cancel.touch()
        with self.assertRaises(a2.Cancelled):
            self.run_import(cancel=cancel)
        self.assertFalse(self.output.exists())
        with patch.object(a2, "check_cancel", side_effect=[None, None, a2.Cancelled()]):
            with self.assertRaises(a2.Cancelled):
                self.run_import()
        self.assertFalse(self.output.exists())
        self.assertTrue((self.source / "cloud.las").is_file())

    def test_cancel_during_conversion_cleans_owned_output_only(self):
        def fail(*args, **kwargs):
            raise a2.Cancelled()
        with patch.object(a2, "convert_cloud", side_effect=fail):
            with self.assertRaises(a2.Cancelled):
                self.run_import()
        self.assertFalse(self.output.exists())
        self.assertTrue(self.source.exists())

    def test_source_changes_during_snapshot_rejected(self):
        source = self.source / "trajectory.txt"
        target = self.root / "snapshot"
        changed = False
        def change(_):
            nonlocal changed
            if not changed:
                changed = True
                source.write_text(source.read_text() + "# change\n")
        with patch.object(a2, "check_cancel", side_effect=change):
            with self.assertRaisesRegex(a2.ImportErrorA2, "изменился"):
                a2.snapshot(source, target, None)

    def test_raw_recordings_do_not_become_fake_completed_imports(self):
        (self.source / "cloud.las").unlink()
        (self.source / "recording.bag").write_bytes(b"raw")
        with self.assertRaisesRegex(a2.ImportErrorA2, "Сначала обработайте"):
            self.run_import()

    def test_cli_and_packaged_style_entrypoint(self):
        command = [sys.executable, str(Path(__file__).with_name("app.py")), "--a2-import-worker",
                   "--input", str(self.source), "--output", str(self.output)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.output / "capture/manifest.json").is_file())
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 1)

    def test_ui_command_keeps_paths_as_arguments(self):
        options = {"source": "C:/my scan", "controls": "C:/points file.csv", "coordinate_frame": "survey-local",
                   "source_version": "v4", "rgb_encoding": "uint16", "up_axis": "z"}
        for frozen in (False, True):
            with patch.object(sys, "frozen", frozen, create=True):
                command = a2_worker_command(options, self.output, self.root / "cancel")
            self.assertEqual(command[command.index("--input") + 1], options["source"])
            self.assertEqual(command[command.index("--controls") + 1], options["controls"])
            if frozen:
                self.assertEqual(command[1], "--a2-import-worker")

    @unittest.skipUnless(shutil.which("node"), "Node.js is not installed")
    def test_real_viewer_reads_imported_color_and_millimetre_spacing(self):
        self.run_import()
        subprocess.run([shutil.which("node"), str(Path(__file__).with_name("test_a2_viewer.cjs")),
                        str(self.output / "cloud.ply")], check=True, timeout=30)

    def ui_options(self):
        return {"source": str(self.source), "destination": str(self.root), "controls": "",
                "coordinate_frame": "test-local", "source_version": "v4", "rgb_encoding": "uint16", "up_axis": "z"}

    def test_ui_launches_import_worker_and_enables_cancel(self):
        ui = Mock(process=None, installing=False, detecting_hardware=False)
        with patch("app.subprocess.Popen") as launch, patch("app.threading.Thread"):
            self.assertTrue(App.start_a2_import(ui, self.ui_options()))
        self.addCleanup(ui.cancel_dir.cleanup)
        self.assertEqual(ui.current_task, "a2")
        self.assertFalse(ui.result_dir.exists())  # worker owns publication
        self.assertIn("--input", launch.call_args.args[0])
        ui.cancel_button.configure.assert_called_with(state="normal")

    def test_ui_failed_launch_releases_cancel_resources(self):
        ui = Mock(process=None, installing=False, detecting_hardware=False)
        with patch("app.subprocess.Popen", side_effect=OSError("cannot launch")), patch("app.messagebox.showerror"):
            self.assertFalse(App.start_a2_import(ui, self.ui_options()))
        self.assertIsNone(ui.cancel_file)
        self.assertIsNone(ui.cancel_dir)

    def test_ui_does_not_open_report_after_failed_import(self):
        import queue
        for exit_code in (0, 1, 130):
            with self.subTest(code=exit_code):
                ui = Mock(current_task="a2", result_dir=self.output, cancel_dir=None, close_after_cancel=False)
                ui.events = queue.Queue()
                ui.events.put(("done", exit_code))
                with patch("app.messagebox.showerror"), patch("app.messagebox.showinfo"):
                    App.read_events(ui)
                self.assertEqual(ui.open_a2_report.call_count, 1 if exit_code == 0 else 0)
                self.assertIsNone(ui.process)


if __name__ == "__main__":
    unittest.main()
