"""Exercise CR2 staging and metadata without depending on a particular camera."""
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import raw_import
import reconstruct


class RawImportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.photos = self.root / "photos"
        self.photos.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def test_cr2_is_found_case_insensitively(self):
        for name in ("a.CR2", "b.cr2", "c.CR2"):
            (self.photos / name).write_bytes(b"source")
        self.assertEqual(len(reconstruct.image_paths(self.photos)), 3)

    def test_working_copies_have_unique_names_and_originals_stay_unchanged(self):
        paths = [self.photos / "same.CR2", self.photos / "same.jpg", self.photos / "other.CR2"]
        for p in paths:
            p.write_bytes(b"source " + p.name.encode())
        original = {p: p.read_bytes() for p in paths}
        output = self.root / "result"
        output.mkdir()
        def convert(source, destination):
            destination.write_bytes(b"developed")
            return {"make": "Canon", "model": "Canon", "focal_length": (50, 1)}
        with patch("raw_import.convert_cr2", side_effect=convert):
            prepared, mapping = raw_import.prepare_images(paths, self.photos, output, lambda: None)
        self.assertEqual([x["prepared"] for x in mapping], ["000000.jpg", "000001.jpg", "000002.jpg"])
        self.assertEqual((prepared / "000001.jpg").read_bytes(), original[paths[1]])
        self.assertEqual({p: p.read_bytes() for p in paths}, original)

    def test_metadata_copies_camera_and_focal_length(self):
        source = self.photos / "sample.CR2"
        source.write_bytes(b"fake")
        ratio = SimpleNamespace(num=40, den=1)
        values = {"Image Make": "Canon", "Image Model": "Canon EOS 5D Mark IV",
                  "EXIF FocalLength": SimpleNamespace(values=[ratio])}
        fake_exif = SimpleNamespace(process_file=lambda *a, **kw: values)
        with patch.dict("sys.modules", {"exifread": fake_exif}):
            metadata = raw_import.read_metadata(source)
        self.assertEqual(metadata["make"], "Canon")
        self.assertEqual(metadata["focal_length"], (40, 1))
        self.assertIn("5D", metadata["model"])


if __name__ == "__main__":
    unittest.main()
