"""Safety and device-identity checks for the experimental OpenCL adapter."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
import zipfile

import colmap_cl_backend as backend
from colmap_cl_install import InstallError, install_archive
from test_amd_opencl_diagnostic import FakePlatform, api, device


class OpenClBackendTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.expected = {"device": "gfx1100", "platform": "Test OpenCL platform", "colmap_gpu_index": 1}

    def tearDown(self):
        self.temp.cleanup()

    def test_selects_gfx1100_on_first_working_platform(self):
        platforms = [FakePlatform([device("gfx1036", 4), device("gfx1100", 4)]),
                     FakePlatform([device("gfx1036", 4), device("gfx1100", 4)])]
        selected = backend.select_target_gpu(api(platforms), object(), lambda *args: None, lambda *args: None)
        self.assertEqual(selected["colmap_gpu_index"], 1)
        self.assertEqual((selected["platform_index"], selected["device_index"]), (0, 1))

    def test_uses_second_platform_when_first_target_fails(self):
        first, second = device("gfx1100", 4), device("gfx1100", 4)
        def verify(cl, np, target):
            if target is first:
                raise RuntimeError("cannot compile")
        selected = backend.select_target_gpu(api([FakePlatform([first]), FakePlatform([second])]),
                                             object(), lambda *args: None, verify)
        self.assertEqual(selected["colmap_gpu_index"], 1000)

    def test_never_chooses_integrated_or_cpu_when_target_missing(self):
        cl = api([FakePlatform([device("gfx1036", 4), device("CPU", 2)])])
        with self.assertRaisesRegex(backend.BackendError, "[Нн]е найдена"):
            backend.select_target_gpu(cl, object(), lambda *args: None, lambda *args: None)

    def run_fake(self, script, expected=None):
        backend.run_program([sys.executable, "-u", "-c", script], self.root, os.environ.copy(),
                            self.root / "colmap-cl.log", lambda: None, expected)

    def test_process_success_requires_matching_platform_device_and_index(self):
        self.run_fake("print('gpu_index: 1'); print('OpenCL platform : Test OpenCL platform'); "
                      "print('OpenCL device : gfx1100')", self.expected)
        self.assertIn("gfx1100", (self.root / "colmap-cl.log").read_text(encoding="utf-8"))
        with self.assertRaisesRegex(backend.BackendError, "не подтвердил"):
            self.run_fake("print('gpu_index: 1'); print('OpenCL platform : Test OpenCL platform'); "
                          "print('OpenCL device : gfx1036')", self.expected)
        with self.assertRaisesRegex(backend.BackendError, "не подтвердил"):
            self.run_fake("print('gpu_index: 1')", self.expected)

    def test_nonzero_exit_is_failure_even_if_device_was_reported(self):
        with self.assertRaisesRegex(backend.BackendError, "кодом"):
            self.run_fake("print('gpu_index: 1'); print('OpenCL platform : Test OpenCL platform'); "
                          "print('OpenCL device : gfx1100'); raise SystemExit(7)", self.expected)

    def test_running_backend_can_be_cancelled(self):
        cancelled = threading.Event()
        class Cancelled(Exception):
            pass
        def check():
            if cancelled.is_set():
                raise Cancelled()
        timer = threading.Timer(0.4, cancelled.set)
        timer.start()
        started = time.monotonic()
        try:
            with self.assertRaises(Cancelled):
                backend.run_program([sys.executable, "-u", "-c", "import time; time.sleep(30)"], self.root,
                                    os.environ.copy(), self.root / "cancel.log", check)
        finally:
            timer.join()
        self.assertLess(time.monotonic() - started, 10)

    def test_installer_rejects_tampering_and_does_not_replace(self):
        archive = self.root / "cl.zip"
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr("bin/colmap.exe", b"binary")
            z.writestr("lib/required.dll", b"dll")
        sha = hashlib.sha256(archive.read_bytes()).hexdigest()
        target = self.root / "engine"
        with self.assertRaises(InstallError):
            install_archive(archive, target, "0" * 64)
        self.assertEqual(install_archive(archive, target, sha), target.resolve())
        self.assertEqual(install_archive(archive, target, sha), target.resolve())
        (target / "bin/colmap.exe").write_bytes(b"changed")
        with self.assertRaises(InstallError):
            install_archive(archive, target, sha)
        self.assertEqual((target / "bin/colmap.exe").read_bytes(), b"changed")

    def test_installer_refuses_path_escape(self):
        archive = self.root / "unsafe.zip"
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr("../escape", b"unsafe")
        with self.assertRaises(InstallError):
            install_archive(archive, self.root / "engine", hashlib.sha256(archive.read_bytes()).hexdigest())
        self.assertFalse((self.root / "escape").exists())


if __name__ == "__main__":
    unittest.main()
