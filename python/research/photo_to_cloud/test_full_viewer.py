"""Run meaningful PLY parser cases from the offline viewer when Node is available."""
from pathlib import Path
import shutil
import subprocess
import unittest


class FullViewerTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "Node.js is not installed")
    def test_binary_ascii_lists_and_truncation(self):
        script = Path(__file__).with_name("test_full_viewer.cjs")
        subprocess.run([shutil.which("node"), str(script)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
