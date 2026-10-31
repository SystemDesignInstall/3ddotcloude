"""Run the viewer's scripts against its real local save endpoint."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from viewer_host import ViewerHost


class ViewerUiTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "Node.js is not installed")
    def test_alignment_is_saved_to_original_with_backup(self):
        header = (b"ply\nformat binary_little_endian 1.0\nelement vertex 4\n"
                  b"property float x\nproperty float y\nproperty float z\n"
                  b"property float nx\nproperty float ny\nproperty float nz\n"
                  b"property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n")
        points = [(0, 0, 0, 0, 1, 0, 255, 0, 0), (2, 0, 0, 0, 1, 0, 0, 255, 0),
                  (0, 1, 0, 0, 1, 0, 0, 0, 255), (2, 1, 0, 0, 1, 0, 255, 255, 0)]
        with tempfile.TemporaryDirectory() as directory:
            cloud = Path(directory) / "room.ply"
            cloud.write_bytes(header + b"".join(struct.pack("<ffffffBBB", *p) for p in points))
            host = ViewerHost(cloud)
            try:
                subprocess.run([shutil.which("node"), str(Path(__file__).with_name("test_full_viewer_app.cjs")),
                                host.url, str(cloud)], check=True, timeout=30)
            finally:
                host.close()


if __name__ == "__main__":
    unittest.main()
