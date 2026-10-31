"""Exercise offline PLY parsing, scaling and rotation when Node is available."""
from pathlib import Path
import shutil
import subprocess
import unittest


class FullViewerTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "Node.js is not installed")
    def test_parser_metric_export_and_rotation(self):
        script = Path(__file__).with_name("test_full_viewer.cjs")
        subprocess.run([shutil.which("node"), str(script)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
