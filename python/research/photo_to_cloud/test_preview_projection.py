"""Verify the saved lightweight preview uses the same camera orientation."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from preview import create_preview


class PreviewProjectionTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "Node.js is not installed")
    def test_projection_and_depth_order(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cloud = root / "cloud.ply"
            cloud.write_bytes(b"ply\nformat ascii 1.0\nelement vertex 2\n"
                              b"property float x\nproperty float y\nproperty float z\n"
                              b"property uchar red\nproperty uchar green\nproperty uchar blue\n"
                              b"end_header\n0 0 0 255 0 0\n1 1 1 0 255 0\n")
            page = root / "preview.html"
            create_preview(cloud, page)
            subprocess.run([shutil.which("node"), str(Path(__file__).with_name("test_preview_projection.cjs")),
                            str(page)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
