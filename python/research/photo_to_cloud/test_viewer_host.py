"""Exercise the local viewer bridge without starting a desktop browser."""
from pathlib import Path
import io
import json
import os
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

import app
from viewer_host import CloudChangedError, CloudSaveSession, InvalidCloudError, ViewerHost


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

    def save_request(self, data, *, origin=True):
        url = self.host.url.split("?", 1)[0]
        parsed = urlsplit(url)
        headers = {"Content-Type": "application/octet-stream", "X-Spatial-Action": "save-current-ply"}
        if origin:
            headers["Origin"] = f"{parsed.scheme}://{parsed.netloc}"
        return urlopen(Request(url + "save", data=data, headers=headers, method="POST"))

    def test_save_replaces_same_file_and_keeps_one_original_backup(self):
        changed = self.payload.replace(b"0 0 0\n", b"1 2 3\n")
        with self.save_request(changed) as response:
            saved = json.load(response)
        backup = self.root / saved["backup"]
        self.assertEqual(self.cloud.read_bytes(), changed)
        self.assertEqual(backup.read_bytes(), self.payload)
        self.assertEqual(backup.parent.name, ".SpatialPhotoCloud-backups")
        changed_again = self.payload.replace(b"0 0 0\n", b"4 5 6\n")
        with self.save_request(changed_again) as response:
            self.assertEqual(json.load(response)["backup"], saved["backup"])
        self.assertEqual(self.cloud.read_bytes(), changed_again)
        self.assertEqual(backup.read_bytes(), self.payload)
        self.assertEqual(len(list(backup.parent.glob("*.ply"))), 1)

    def test_save_rejects_cross_origin_and_wrong_structure(self):
        changed = self.payload.replace(b"0 0 0\n", b"1 2 3\n")
        with self.assertRaises(HTTPError) as error:
            self.save_request(changed, origin=False)
        self.assertEqual(error.exception.code, 403)
        wrong = changed.replace(b"property float x", b"property float other")
        with self.assertRaises(HTTPError) as error:
            self.save_request(wrong)
        self.assertEqual(error.exception.code, 400)
        self.assertEqual(self.cloud.read_bytes(), self.payload)
        self.assertFalse((self.root / ".SpatialPhotoCloud-backups").exists())

    def test_save_detects_external_change(self):
        external = self.payload.replace(b"0 0 0\n", b"7 8 9\n")
        previous = self.cloud.stat().st_mtime_ns
        self.cloud.write_bytes(external)
        os.utime(self.cloud, ns=(previous + 10_000_000, previous + 10_000_000))
        with self.assertRaises(HTTPError) as error:
            self.save_request(self.payload)
        self.assertEqual(error.exception.code, 409)
        self.assertEqual(self.cloud.read_bytes(), external)

    def test_removed_backup_is_recreated_before_next_save(self):
        changed = self.payload.replace(b"0 0 0\n", b"1 2 3\n")
        with self.save_request(changed) as response:
            backup = self.root / json.load(response)["backup"]
        backup.unlink()
        changed_again = self.payload.replace(b"0 0 0\n", b"4 5 6\n")
        with self.save_request(changed_again) as response:
            new_backup = self.root / json.load(response)["backup"]
        self.assertEqual(new_backup.read_bytes(), changed)
        self.assertEqual(self.cloud.read_bytes(), changed_again)

    def test_interrupted_write_or_replace_failure_preserves_original(self):
        saver = CloudSaveSession(self.cloud)
        with self.assertRaises(InvalidCloudError):
            saver.save(io.BytesIO(self.payload[:20]), len(self.payload))
        self.assertEqual(self.cloud.read_bytes(), self.payload)
        self.assertFalse(list(self.root.glob(".spatial-save-*")))
        changed = self.payload.replace(b"0 0 0\n", b"1 2 3\n")
        with patch("viewer_host.os.replace", side_effect=PermissionError("locked")):
            with self.assertRaises(PermissionError):
                saver.save(io.BytesIO(changed), len(changed))
        self.assertEqual(self.cloud.read_bytes(), self.payload)
        self.assertEqual(saver.backup.read_bytes(), self.payload)
        self.assertFalse(list(self.root.glob(".spatial-save-*")))

    def test_change_during_write_is_not_overwritten(self):
        saver = CloudSaveSession(self.cloud)
        changed = self.payload.replace(b"0 0 0\n", b"1 2 3\n")
        external = self.payload.replace(b"0 0 0\n", b"9 8 7\n")
        def external_edit(_source, _candidate):
            self.cloud.write_bytes(external)
            os.utime(self.cloud, ns=(saver.snapshot[1] + 10_000_000,) * 2)
        with patch("viewer_host._check_replacement", side_effect=external_edit):
            with self.assertRaises(CloudChangedError):
                saver.save(io.BytesIO(changed), len(changed))
        self.assertEqual(self.cloud.read_bytes(), external)
        self.assertFalse(list(self.root.glob(".spatial-save-*")))


if __name__ == "__main__":
    unittest.main()
