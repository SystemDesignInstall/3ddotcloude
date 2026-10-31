from pathlib import Path
import hashlib
import struct
import sys
import tempfile
import threading
import time
import unittest
import zipfile

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
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        output = self.root / "installed"
        with self.assertRaises(InstallError):
            install_archive(archive, output, "0" * 64)
        self.assertFalse(output.exists())
        self.assertEqual(install_archive(archive, output, digest), output.resolve())
        self.assertEqual((output / "InterfaceCOLMAP.exe").read_bytes(), b"test converter")
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


if __name__ == "__main__":
    unittest.main()
