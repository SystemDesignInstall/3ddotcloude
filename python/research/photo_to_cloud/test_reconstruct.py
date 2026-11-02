from pathlib import Path
from contextlib import closing
import hashlib
import json
import sqlite3
import struct
import sys
import tempfile
import threading
import time
import unittest
import zipfile
from types import SimpleNamespace
from unittest.mock import patch

from PIL import Image

import reconstruct
from openmvs_install import InstallError, PREFIX, install_archive
from preview import create_preview, sample_ply


class PhotoCloudSafetyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.images = self.root / "photos"
        self.images.mkdir()
        for index in range(3):
            Image.new("RGB", (16, 16), (index * 40, 10, 20)).save(self.images / f"{index}.png")

    def tearDown(self):
        self.tmp.cleanup()

    def test_recommended_threads_respects_cpu_and_ram_budgets(self):
        gib = 1024 ** 3
        self.assertEqual(reconstruct.recommended_threads(32, 64 * gib, 16), 16)
        self.assertEqual(reconstruct.recommended_threads(32, 32 * gib, 16), 8)
        self.assertEqual(reconstruct.recommended_threads(16, 64 * gib, 8), 8)
        self.assertEqual(reconstruct.recommended_threads(8, 64 * gib, 4), 4)
        self.assertEqual(reconstruct.recommended_threads(32, 2 * gib, 16), 1)
        self.assertEqual(reconstruct.recommended_threads(1, 64 * gib, 1), 1)

    def test_worker_command_passes_explicit_threads(self):
        import app

        command = app.worker_command("photos", self.root / "result", "sparse-cpu",
                                    self.root / "cancel", 16)
        self.assertEqual(command[command.index("--threads") + 1], "16")

    def test_texture_worker_command_preserves_paths_and_thread_count(self):
        import app

        command = app.texture_worker_command(self.root / "mesh.ply", self.root / "dense_workspace",
                                             self.root / "openmvs", self.root / "model.glb",
                                             self.root / "cancel", 12)
        self.assertEqual(command[command.index("--mesh") + 1], str(self.root / "mesh.ply"))
        self.assertEqual(command[command.index("--workspace") + 1], str(self.root / "dense_workspace"))
        self.assertEqual(command[command.index("--threads") + 1], "12")

    def test_verified_match_statistics_count_only_geometric_inliers(self):
        database = self.root / "database.db"
        with closing(sqlite3.connect(database)) as connection, connection:
            connection.execute("CREATE TABLE two_view_geometries (pair_id INTEGER, rows INTEGER, cols INTEGER, data BLOB)")
            connection.executemany("INSERT INTO two_view_geometries VALUES (?, ?, 2, X'00')",
                                   [(1, 24), (2, 0), (3, 9)])
        self.assertEqual(reconstruct.verified_match_statistics(database),
                         {"verified_image_pairs": 2, "verified_inlier_matches": 33})

    def _matching_database(self):
        database = self.root / "matching.db"
        with closing(sqlite3.connect(database)) as connection, connection:
            connection.execute("CREATE TABLE two_view_geometries (pair_id INTEGER, rows INTEGER, cols INTEGER, data BLOB)")
        return database

    def test_opencl_matcher_is_kept_only_after_verified_pairs_exist(self):
        database = self._matching_database()
        cpu_calls = []
        def opencl_match():
            with closing(sqlite3.connect(database)) as connection, connection:
                connection.execute("INSERT INTO two_view_geometries VALUES (1, 17, 2, X'00')")
        result = reconstruct.match_with_opencl_fallback(database, opencl_match,
                                                        lambda: cpu_calls.append(True), logger=lambda _: None)
        self.assertEqual(result["status"], "verified")
        self.assertEqual(result["backend"], "COLMAP-CL OpenCL")
        self.assertEqual(result["statistics"]["verified_inlier_matches"], 17)
        self.assertEqual(cpu_calls, [])
        self.assertFalse(database.with_name(database.name + ".before-opencl-matching").exists())

    def test_opencl_matcher_partial_failure_restores_database_before_cpu_fallback(self):
        database = self._matching_database()
        def opencl_match():
            with closing(sqlite3.connect(database)) as connection, connection:
                connection.execute("INSERT INTO two_view_geometries VALUES (1, 99, 2, X'00')")
            raise RuntimeError("OpenCL matcher crashed after a partial write")
        def cpu_match():
            with closing(sqlite3.connect(database)) as connection, connection:
                connection.execute("INSERT INTO two_view_geometries VALUES (2, 23, 2, X'00')")
        result = reconstruct.match_with_opencl_fallback(database, opencl_match, cpu_match, logger=lambda _: None)
        with closing(sqlite3.connect(database)) as connection, connection:
            rows = connection.execute("SELECT pair_id, rows FROM two_view_geometries").fetchall()
        self.assertEqual(result["status"], "cpu_fallback")
        self.assertEqual(result["statistics"]["verified_inlier_matches"], 23)
        self.assertEqual(rows, [(2, 23)])

    def test_opencl_matcher_without_verified_pairs_restores_before_cpu_fallback(self):
        database = self._matching_database()
        cpu_calls = []
        def opencl_match():
            with closing(sqlite3.connect(database)) as connection, connection:
                connection.execute("INSERT INTO two_view_geometries VALUES (1, 0, 2, X'00')")
        def cpu_match():
            cpu_calls.append(True)
            with closing(sqlite3.connect(database)) as connection, connection:
                connection.execute("INSERT INTO two_view_geometries VALUES (2, 19, 2, X'00')")
        result = reconstruct.match_with_opencl_fallback(database, opencl_match, cpu_match, logger=lambda _: None)
        with closing(sqlite3.connect(database)) as connection, connection:
            rows = connection.execute("SELECT pair_id, rows FROM two_view_geometries").fetchall()
        self.assertEqual(result["status"], "cpu_fallback")
        self.assertEqual(result["statistics"]["verified_inlier_matches"], 19)
        self.assertEqual(cpu_calls, [True])
        self.assertEqual(rows, [(2, 19)])

    def test_opencl_matching_cancellation_restores_database_without_cpu_fallback(self):
        database = self._matching_database()
        cpu_calls = []
        def opencl_match():
            with closing(sqlite3.connect(database)) as connection, connection:
                connection.execute("INSERT INTO two_view_geometries VALUES (1, 99, 2, X'00')")
            raise reconstruct.ReconstructionCancelled("cancelled")
        with self.assertRaises(reconstruct.ReconstructionCancelled):
            reconstruct.match_with_opencl_fallback(database, opencl_match,
                                                   lambda: cpu_calls.append(True), logger=lambda _: None)
        self.assertEqual(reconstruct.verified_match_statistics(database),
                         {"verified_image_pairs": 0, "verified_inlier_matches": 0})
        self.assertEqual(cpu_calls, [])

    def test_no_verified_overlap_is_a_clear_quality_failure(self):
        quality = reconstruct.failed_reconstruction_quality(
            ["first.jpg", "second.jpg"],
            {"verified_image_pairs": 0, "verified_inlier_matches": 0},
            "Нет ни одной геометрически подтверждённой пары фотографий.",
        )
        self.assertEqual(quality["assessment"], "rejected")
        self.assertFalse(quality["dense_reconstruction_allowed"])
        self.assertEqual(quality["unregistered_images"], ["first.jpg", "second.jpg"])
        self.assertEqual(quality["verified_image_pairs"], 0)

    def test_quality_gate_blocks_dense_cloud_for_low_registration_and_bad_fit(self):
        class FakeImage:
            def __init__(self, name, image_id):
                self.name, self.image_id = name, image_id
                self.num_points3D = 20
            def num_points2D(self):
                return 100
            def projection_center(self):
                return (self.image_id, 0, 0)
            def viewing_direction(self):
                return (0, 0, 1)

        class FakeModel:
            def __init__(self):
                self._images = {1: FakeImage("a.jpg", 1), 2: FakeImage("b.jpg", 2)}
                self.points3D = {index: SimpleNamespace(error=9.5) for index in range(10)}
            def reg_image_ids(self):
                return list(self._images)
            def image(self, image_id):
                return self._images[image_id]
            def num_points3D(self):
                return len(self.points3D)
            def compute_mean_reprojection_error(self):
                return 9.5
            def compute_mean_track_length(self):
                return 2.0
            def compute_mean_observations_per_reg_image(self):
                return 50.0

        quality, centers = reconstruct.assess_reconstruction_quality(
            FakeModel(), [f"{name}.jpg" for name in "abcdefghij"],
            {"verified_image_pairs": 1, "verified_inlier_matches": 30}, 1)
        self.assertEqual(quality["assessment"], "rejected")
        self.assertFalse(quality["dense_reconstruction_allowed"])
        self.assertEqual(quality["registered_images"], 2)
        self.assertEqual(len(quality["unregistered_images"]), 8)
        self.assertEqual(len(centers), 2)

    def test_quality_report_maps_prepared_raw_names_to_source_photos(self):
        class FakeImage:
            image_id = 1
            name = "000001.jpg"
            num_points3D = 30
            def num_points2D(self):
                return 120
            def projection_center(self):
                return (1.0, 2.0, 3.0)
            def viewing_direction(self):
                return (0.0, 0.0, 1.0)

        class FakeModel:
            images = {1: FakeImage()}
            points3D = {index: SimpleNamespace(error=0.8) for index in range(10)}
            def reg_image_ids(self):
                return [1]
            def image(self, image_id):
                return self.images[image_id]
            def num_points3D(self):
                return len(self.points3D)
            def compute_mean_reprojection_error(self):
                return 0.8
            def compute_mean_track_length(self):
                return 3.0
            def compute_mean_observations_per_reg_image(self):
                return 30.0

        quality, centers = reconstruct.assess_reconstruction_quality(
            FakeModel(), ["IMG_1873.CR2"],
            {"verified_image_pairs": 2, "verified_inlier_matches": 120}, 1,
            [{"prepared": "000001.jpg", "source": "IMG_1873.CR2"}])
        self.assertEqual(quality["unregistered_images"], [])
        self.assertEqual(centers[0]["image"], "IMG_1873.CR2")
        csv_path = self.root / "camera_centers.csv"
        reconstruct.write_camera_centers(csv_path, centers)
        self.assertIn(b"IMG_1873.CR2", csv_path.read_bytes())

    def test_quality_report_rejects_empty_model_and_serializes_nonfinite_metrics_safely(self):
        class FakeImage:
            name = "bad.jpg"
            num_points3D = 0
            def num_points2D(self):
                return 0
            def projection_center(self):
                return (float("nan"), 0.0, 0.0)
            def viewing_direction(self):
                return (0.0, float("inf"), 1.0)

        class FakeModel:
            points3D = {}
            def reg_image_ids(self):
                return [1]
            def image(self, image_id):
                return FakeImage()
            def num_points3D(self):
                return 0
            def compute_mean_reprojection_error(self):
                return float("nan")
            def compute_mean_track_length(self):
                return float("inf")
            def compute_mean_observations_per_reg_image(self):
                return float("nan")

        quality, centers = reconstruct.assess_reconstruction_quality(
            FakeModel(), ["bad.jpg"], {"verified_image_pairs": 1, "verified_inlier_matches": 20}, 1)
        self.assertEqual(quality["assessment"], "rejected")
        self.assertFalse(quality["dense_reconstruction_allowed"])
        self.assertIsNone(quality["mean_reprojection_error_px"])
        self.assertIsNone(quality["mean_track_length"])
        self.assertIsNone(quality["mean_observations_per_registered_image"])
        self.assertIsNone(centers[0]["center"][0])
        self.assertIsNone(centers[0]["direction"][1])
        json.dumps({"quality": quality, "centers": centers}, allow_nan=False)

    def test_existing_output_is_never_overwritten(self):
        output = self.root / "existing"
        output.mkdir()
        marker = output / "keep.txt"
        marker.write_text("keep", encoding="utf-8")
        with self.assertRaisesRegex(reconstruct.ReconstructionError, "уже существует"):
            reconstruct.reconstruct(self.images, output, "sparse-cpu", 1)
        self.assertEqual(marker.read_text(encoding="utf-8"), "keep")

    def test_result_cannot_be_inside_input_folder(self):
        with self.assertRaisesRegex(reconstruct.ReconstructionError, "вне папки фотографий"):
            reconstruct.reconstruct(self.images, self.images / "result", "sparse-cpu", 1)
        self.assertFalse((self.images / "result").exists())

    def test_corrupt_image_fails_before_creating_output(self):
        (self.images / "1.png").write_bytes(b"not a png")
        output = self.root / "result"
        with self.assertRaisesRegex(reconstruct.ReconstructionError, "Не удалось прочитать"):
            reconstruct.reconstruct(self.images, output, "sparse-cpu", 1)
        self.assertFalse(output.exists())

    def test_missing_openmvs_fails_before_creating_output(self):
        output = self.root / "result"
        with self.assertRaisesRegex(reconstruct.ReconstructionError, "OpenMVS"):
            reconstruct.reconstruct(self.images, output, "dense-cpu", 1, self.root / "missing")
        self.assertFalse(output.exists())

    def test_ply_must_have_nonzero_vertex_count_and_complete_header(self):
        ply = self.root / "cloud.ply"
        for content in (b"garbage\n", b"ply\nelement vertex 0\nend_header\n", b"ply\nelement vertex 7\n",
                        b"ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nend_header\n"):
            ply.write_bytes(content)
            with self.assertRaises(reconstruct.ReconstructionError):
                reconstruct.ply_vertex_count(ply)
        ply.write_bytes(b"ply\nformat ascii 1.0\nelement vertex 7\nproperty float x\nend_header\n" + b"1\n" * 7)
        self.assertEqual(reconstruct.ply_vertex_count(ply), 7)

    def test_ply_rejects_truncated_binary_variable_lists(self):
        ply = self.root / "cloud.ply"
        header = (b"ply\nformat binary_little_endian 1.0\nelement vertex 1\n"
                  b"property float x\nproperty list uint8 uint32 views\nend_header\n")
        ply.write_bytes(header + b"\x00\x00\x00\x00\x02\x00\x00\x00\x00")
        with self.assertRaises(reconstruct.ReconstructionError):
            reconstruct.ply_vertex_count(ply)
        with ply.open("ab") as stream:
            stream.write(b"\x00\x00\x00\x00")
        self.assertEqual(reconstruct.ply_vertex_count(ply), 1)

    def test_backend_exit_failure_is_not_reported_as_success(self):
        with self.assertRaisesRegex(reconstruct.ReconstructionError, "кодом 9"):
            reconstruct.run_program([sys.executable, "-c", "raise SystemExit(9)"], self.root)

    def test_windows_launcher_has_crlf_lines(self):
        data = Path(__file__).with_name("start-windows.bat").read_bytes()
        self.assertIn(b"\r\n", data)
        self.assertNotIn(b"\n", data.replace(b"\r\n", b""))

    def test_active_backend_can_be_stopped(self):
        marker = self.root / "cancel"
        timer = threading.Timer(0.3, lambda: marker.write_text("cancel", encoding="ascii"))
        timer.start()
        started = time.monotonic()
        try:
            with self.assertRaises(reconstruct.ReconstructionCancelled):
                reconstruct.run_program([sys.executable, "-c", "import time; time.sleep(30)"], self.root, marker)
        finally:
            timer.join()
        self.assertLess(time.monotonic() - started, 10)

    def test_preview_is_offline_and_does_not_replace_ply(self):
        ply = self.root / "cloud.ply"
        data = (b"ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nproperty float y\n"
                b"property float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\n"
                b"end_header\n1 2 3 10 20 30\n4 5 6 40 50 60\n")
        ply.write_bytes(data)
        html = self.root / "preview.html"
        self.assertEqual(create_preview(ply, html), 2)
        text = html.read_text(encoding="utf-8")
        self.assertIn("Показано 2 из 2 точек", text)
        self.assertIn('href="full_viewer.html"', text)
        self.assertTrue((self.root / "full_viewer.html").is_file())
        self.assertNotIn("https://", text)
        self.assertEqual(ply.read_bytes(), data)

    def test_preview_reads_binary_vertices_with_variable_lists(self):
        ply = self.root / "cloud.ply"
        header = (b"ply\nformat binary_little_endian 1.0\nelement vertex 2\nproperty float x\n"
                  b"property float y\nproperty float z\nproperty uchar red\nproperty uchar green\n"
                  b"property uchar blue\nproperty list uchar uint32 views\nend_header\n")
        vertex1 = struct.pack("<fffBBBBII", 1, 2, 3, 10, 20, 30, 2, 8, 9)
        vertex2 = struct.pack("<fffBBBBI", 4, 5, 6, 40, 50, 60, 1, 5)
        ply.write_bytes(header + vertex1 + vertex2)
        count, sample = sample_ply(ply, limit=1)
        self.assertEqual(count, 2)
        self.assertEqual(sample, [[1., 2., 3., 10, 20, 30]])

    def test_installer_verifies_archive_and_does_not_overwrite(self):
        archive = self.root / "openmvs.zip"
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr(PREFIX + "InterfaceCOLMAP.exe", b"test converter")
            z.writestr(PREFIX + "DensifyPointCloud.exe", b"test densifier")
            z.writestr(PREFIX + "TextureMesh.exe", b"test texturer")
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        output = self.root / "installed"
        with self.assertRaises(InstallError):
            install_archive(archive, output, "0" * 64)
        self.assertFalse(output.exists())
        self.assertEqual(install_archive(archive, output, digest), output.resolve())
        self.assertEqual((output / "InterfaceCOLMAP.exe").read_bytes(), b"test converter")
        self.assertEqual((output / "TextureMesh.exe").read_bytes(), b"test texturer")
        self.assertEqual(install_archive(archive, output, digest), output.resolve())
        (output / "InterfaceCOLMAP.exe").write_bytes(b"modified")
        with self.assertRaises(InstallError):
            install_archive(archive, output, digest)
        other = self.root / "existing"; other.mkdir()
        (other / "keep").write_text("keep", encoding="ascii")
        with self.assertRaises(InstallError):
            install_archive(archive, other, digest)
        self.assertEqual((other / "keep").read_text(encoding="ascii"), "keep")

    def test_installer_rejects_archive_path_escape(self):
        archive = self.root / "unsafe.zip"
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr(PREFIX + "../escape.exe", b"escape")
        output = self.root / "installed"
        with self.assertRaises(InstallError):
            install_archive(archive, output, hashlib.sha256(archive.read_bytes()).hexdigest())
        self.assertFalse(output.exists())
        self.assertFalse((self.root / "escape.exe").exists())

    def test_installer_requires_texturemesh_for_complete_openmvs_package(self):
        archive = self.root / "openmvs-without-texturer.zip"
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr(PREFIX + "InterfaceCOLMAP.exe", b"test converter")
            z.writestr(PREFIX + "DensifyPointCloud.exe", b"test densifier")
        output = self.root / "installed"
        with self.assertRaisesRegex(InstallError, "TextureMesh"):
            install_archive(archive, output, hashlib.sha256(archive.read_bytes()).hexdigest())
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
