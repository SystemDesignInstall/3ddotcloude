"""Tests for the isolated Poisson mesh stage without requiring native PyCOLMAP."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import mesh_worker


INPUT_PLY = (
    "ply\nformat ascii 1.0\n"
    "comment SpatialPhotoCloud units meters\n"
    "comment SpatialPhotoCloud up z\n"
    "element vertex 3\n"
    "property float x\nproperty float y\nproperty float z\n"
    "property float nx\nproperty float ny\nproperty float nz\n"
    "property uchar red\nproperty uchar green\nproperty uchar blue\n"
    "end_header\n0 0 0 0 0 1 255 0 0\n1 0 0 0 0 1 0 255 0\n"
    "0 1 0 0 0 1 0 0 255\n"
).encode("ascii")

OUTPUT_PLY = (
    "ply\nformat ascii 1.0\n"
    "element vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
    "property uchar red\nproperty uchar green\nproperty uchar blue\n"
    "element face 1\nproperty list uchar int vertex_indices\n"
    "end_header\n0 0 0 255 0 0\n1 0 0 0 255 0\n0 1 0 0 0 255\n3 0 1 2\n"
).encode("ascii")


class MeshWorkerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.source = self.root / "dense.ply"
        self.source.write_bytes(INPUT_PLY)
        self.output = self.root / "dense-mesh.ply"

    def tearDown(self) -> None:
        self.temp.cleanup()

    def fake_pycolmap(self, mesher):
        class Options:
            def __init__(self):
                self.depth = None
                self.trim = None
                self.num_threads = None
                self.color = None

        return types.SimpleNamespace(
            __version__="4.2.1-test",
            PoissonMeshingOptions=Options,
            poisson_meshing=mesher,
        )

    def test_builds_a_separate_colored_mesh_and_preserves_metric_metadata(self):
        original = self.source.read_bytes()
        captured = {}

        def mesher(*, input_path, output_path, options):
            captured.update(input=input_path, output=output_path, options=options)
            Path(output_path).write_bytes(OUTPUT_PLY)

        module = self.fake_pycolmap(mesher)
        with patch.dict(sys.modules, {"pycolmap": module}):
            report = mesh_worker.build_mesh(self.source, self.output, threads=8, depth=13)

        self.assertEqual(self.source.read_bytes(), original)
        self.assertTrue(self.output.is_file())
        result = self.output.read_bytes()
        self.assertIn(b"comment SpatialPhotoCloud units meters\n", result)
        self.assertIn(b"comment SpatialPhotoCloud up z\n", result)
        self.assertEqual(captured["input"], str(self.source.resolve()))
        self.assertTrue(captured["output"].endswith(".partial.ply"))
        self.assertEqual(captured["options"].depth, 13)
        self.assertEqual(captured["options"].trim, 10.0)
        self.assertEqual(captured["options"].num_threads, 8)
        self.assertTrue(captured["options"].color)
        self.assertEqual(report["mesh_faces"], 1)
        self.assertEqual(report["scale"], "meters")
        self.assertEqual(report["up_axis"], "z")
        sidecar = json.loads(Path(str(self.output) + ".report.json").read_text(encoding="utf-8"))
        self.assertEqual(sidecar["compute_backend"], "CPU")

    def test_refuses_to_replace_source_or_an_existing_destination(self):
        with self.assertRaisesRegex(mesh_worker.MeshError, "отдельный файл"):
            mesh_worker.build_mesh(self.source, self.source, threads=1)
        self.output.write_bytes(b"keep")
        with self.assertRaisesRegex(mesh_worker.MeshError, "уже существует"):
            mesh_worker.build_mesh(self.source, self.output, threads=1)
        self.assertEqual(self.output.read_bytes(), b"keep")

    def test_refuses_to_reuse_an_existing_report_name(self):
        report = Path(str(self.output) + ".report.json")
        report.write_text("keep", encoding="utf-8")
        with self.assertRaisesRegex(mesh_worker.MeshError, "Отчёт для этого результата уже существует"):
            mesh_worker.build_mesh(self.source, self.output, threads=1)
        self.assertFalse(self.output.exists())
        self.assertEqual(report.read_text(encoding="utf-8"), "keep")

    def test_requires_oriented_normals_and_valid_settings(self):
        self.source.write_bytes(INPUT_PLY.replace(b"property float nx\nproperty float ny\nproperty float nz\n", b""))
        with self.assertRaisesRegex(mesh_worker.MeshError, "нет нормалей"):
            mesh_worker.build_mesh(self.source, self.output, threads=1)
        self.source.write_bytes(INPUT_PLY)
        with self.assertRaisesRegex(mesh_worker.MeshError, "от 1 до 64"):
            mesh_worker.build_mesh(self.source, self.output, threads=0)
        with self.assertRaisesRegex(mesh_worker.MeshError, "от 8 до 15"):
            mesh_worker.build_mesh(self.source, self.output, threads=1, depth=16)

    def test_refuses_to_feed_an_existing_surface_back_into_poisson(self):
        self.source.write_bytes(INPUT_PLY.replace(
            b"end_header\n",
            b"element face 1\nproperty list uchar int vertex_indices\nend_header\n",
            1,
        ))
        with self.assertRaisesRegex(mesh_worker.MeshError, "исходное облако точек без граней"):
            mesh_worker.build_mesh(self.source, self.output, threads=1)

    def test_rejects_output_without_faces_and_keeps_no_partial_file(self):
        def mesher(*, input_path, output_path, options):
            Path(output_path).write_bytes(INPUT_PLY)

        with patch.dict(sys.modules, {"pycolmap": self.fake_pycolmap(mesher)}):
            with self.assertRaisesRegex(mesh_worker.MeshError, "не содержит треугольную поверхность"):
                mesh_worker.build_mesh(self.source, self.output, threads=1)
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.root.glob("*.partial.ply")), [])


if __name__ == "__main__":
    unittest.main()
