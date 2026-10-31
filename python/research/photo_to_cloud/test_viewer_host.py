"""Exercise the local viewer bridge without starting a desktop browser."""
from pathlib import Path
import json
import tempfile
import unittest
from urllib.error import HTTPError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

import app
from viewer_host import ViewerHost


class ViewerHostTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.cloud = self.root / "зала.ply"
        self.payload = b"ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n"
        self.cloud.write_bytes(self.payload)
        self.host = ViewerHost(self.cloud)

    def tearDown(self):
        self.host.close()
        self.directory.cleanup()

    def test_only_chosen_file_is_available_on_random_local_path(self):
        url = self.host.url.split("?", 1)[0]
        self.assertEqual(self.host.server.server_address[0], "127.0.0.1")
        with urlopen(url) as response:
            self.assertIn(b"Spatial Photo Cloud", response.read())
            self.assertEqual(response.headers["Cache-Control"], "no-store")
        with urlopen(url + "meta.json") as response:
            self.assertEqual(json.load(response)["name"], self.cloud.name)
        with urlopen(url + "cloud") as response:
            self.assertEqual(response.read(), self.payload)
        parsed = urlsplit(url)
        for denied in (f"{parsed.scheme}://{parsed.netloc}/cloud", url + "../cloud", url + "other.ply"):
            with self.assertRaises(HTTPError) as error:
                urlopen(denied)
            self.assertEqual(error.exception.code, 404)
        with self.assertRaises(HTTPError):
            urlopen(Request(url, headers={"Host": "attacker.invalid"}))

    def test_old_result_can_be_opened_without_preview_html(self):
        fake_app = app.App.__new__(app.App)
        fake_app.result_dir = self.root
        (self.root / "sparse.ply").write_bytes(self.payload)
        self.assertEqual(fake_app.result_cloud().name, "sparse.ply")
        (self.root / "dense.ply").write_bytes(self.payload)
        self.assertEqual(fake_app.result_cloud().name, "dense.ply")

    def test_non_ply_cannot_be_served_as_cloud(self):
        other = self.root / "other.txt"
        other.write_text("no", encoding="utf-8")
        with self.assertRaises(ValueError):
            ViewerHost(other)


if __name__ == "__main__":
    unittest.main()
