"""Tests for textured GLB export without requiring the native OpenMVS binaries."""

from __future__ import annotations

import json
import os
from pathlib import Path
import struct
import tempfile
import unittest

import texture_worker
from mesh_worker import MeshError


MESH_PLY = (
    b"ply\nformat ascii 1.0\nelement vertex 3\n"
    b"property float x\nproperty float y\nproperty float z\n"
    b"element face 1\nproperty list uchar int vertex_indices\n"
    b"end_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n"
)


def make_glb(document: dict | None = None, binary: bytes | None = None) -> bytes:
    document = document or {
        "asset": {"version": "2.0"},
        "images": [{"uri": "data:image/png;base64,AA=="}],
        "textures": [{"source": 0}],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
    }
    json_chunk = json.dumps(document, separators=(",", ":")).encode("utf-8")
    json_chunk += b" " * (-len(json_chunk) % 4)
    chunks = [struct.pack("<II", len(json_chunk), texture_worker.GLB_JSON_CHUNK) + json_chunk]
    if binary is not None:
        padded_binary = binary + b"\0" * (-len(binary) % 4)
        chunks.append(struct.pack("<II", len(padded_binary), 0x004E4942) + padded_binary)
    total_size = 12 + sum(map(len, chunks))
    return struct.pack("<4sII", b"glTF", 2, total_size) + b"".join(chunks)


def make_external_glb(uri: str = "textured-model_0.png") -> bytes:
    return make_glb({
        "asset": {"version": "2.0"},
        "images": [{"uri": uri, "mimeType": "image/png"}],
        "textures": [{"source": 0}],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
        "buffers": [{"byteLength": 4}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 4}],
    }, binary=b"GEOM")


def glb_document(path: Path) -> dict:
    with path.open("rb") as stream:
        stream.seek(12)
        chunk_size, chunk_type = struct.unpack("<II", stream.read(8))
        if chunk_type != texture_worker.GLB_JSON_CHUNK:
            raise AssertionError("JSON is not the first GLB chunk")
        return json.loads(stream.read(chunk_size).decode("utf-8").rstrip(" \t\r\n\0"))


class TextureWorkerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.mesh = self.root / "mesh.ply"
        self.mesh.write_bytes(MESH_PLY)
        self.result = self.root / "result"
        self.workspace = self.result / "dense_workspace"
        self.images = self.workspace / "images"
        self.images.mkdir(parents=True)
        sparse = self.workspace / "sparse" / "0"
        sparse.mkdir(parents=True)
        (sparse / "cameras.bin").write_bytes(b"camera")
        (sparse / "images.bin").write_bytes(b"images")
        for index in range(3):
            (self.images / f"{index}.jpg").write_bytes(b"image")
        self.openmvs = self.root / "openmvs"
        self.openmvs.mkdir()
        suffix = ".exe" if os.name == "nt" else ""
        for name in ("InterfaceCOLMAP", "TextureMesh"):
            executable = self.openmvs / (name + suffix)
            executable.write_text("test double", encoding="ascii")
            executable.chmod(0o755)
        self.output = self.root / "model.glb"

    def tearDown(self) -> None:
        self.temp.cleanup()

    def runner_for(self, glb: bytes | None = None, cancel_on_interface: Path | None = None):
        calls = []
        working_directories = []

        def runner(args, cwd, cancel_file):
            calls.append(args)
            working_directories.append(cwd)
            if Path(args[0]).stem == "InterfaceCOLMAP":
                output = Path(args[args.index("--output-file") + 1])
                output.write_bytes(b"openmvs scene")
                if cancel_on_interface is not None:
                    cancel_on_interface.write_text("cancel", encoding="ascii")
            else:
                output = (cwd / args[args.index("--output-file") + 1]).with_suffix(".glb")
                payload = glb if glb is not None else make_external_glb()
                output.write_bytes(payload)
                document = glb_document(output)
                for image in document.get("images", []):
                    uri = image.get("uri", "")
                    if uri and not uri.startswith("data:") and ".." not in Path(uri).parts:
                        (cwd / uri).write_bytes(b"fake PNG texture")
        return calls, working_directories, runner

    def test_creates_standalone_textured_glb_from_matching_workspace(self):
        original_mesh = self.mesh.read_bytes()
        calls, working_directories, runner = self.runner_for()
        report = texture_worker.build_textured_model(self.mesh, self.result, self.openmvs,
                                                      self.output, threads=8, runner=runner)
        self.assertTrue(self.output.is_file())
        self.assertEqual(self.mesh.read_bytes(), original_mesh)
        self.assertEqual(report["status"], "succeeded")
        self.assertEqual(report["registered_images"], 3)
        self.assertEqual(report["glb_textures"], 1)
        document = glb_document(self.output)
        self.assertNotIn("uri", document["images"][0])
        view = document["bufferViews"][document["images"][0]["bufferView"]]
        self.assertGreaterEqual(view["byteOffset"], 4)
        self.assertEqual(document["buffers"][0]["byteLength"], view["byteOffset"] + view["byteLength"])
        self.assertEqual([Path(call[0]).stem for call in calls], ["InterfaceCOLMAP", "TextureMesh"])
        self.assertIn("--image-folder", calls[0])
        self.assertIn("--mesh-file", calls[1])
        self.assertEqual(calls[1][calls[1].index("--export-type") + 1], "glb")
        self.assertEqual(calls[1][calls[1].index("--output-file") + 1], "textured-model")
        self.assertEqual(len(set(working_directories)), 1)
        self.assertEqual(calls[1][calls[1].index("--working-folder") + 1], str(working_directories[1]))
        self.assertEqual(list(self.root.glob("spc-texture-*")), [])

    def test_rejects_point_cloud_and_missing_camera_workspace(self):
        point_cloud = self.root / "points.ply"
        point_cloud.write_bytes(MESH_PLY.replace(b"element face 1\nproperty list uchar int vertex_indices\n", b""))
        calls, _, runner = self.runner_for()
        with self.assertRaisesRegex(MeshError, "не содержит треугольную поверхность"):
            texture_worker.build_textured_model(point_cloud, self.result, self.openmvs,
                                                self.output, threads=1, runner=runner)
        (self.workspace / "sparse" / "0" / "cameras.bin").unlink()
        (self.workspace / "sparse" / "0" / "images.bin").unlink()
        with self.assertRaisesRegex(texture_worker.TextureError, "модель камер COLMAP"):
            texture_worker.build_textured_model(self.mesh, self.result, self.openmvs,
                                                self.output, threads=1, runner=runner)
        self.assertEqual(calls, [])
        self.assertFalse(self.output.exists())

    def test_rejects_invalid_glb_without_publishing_a_file(self):
        calls, _, runner = self.runner_for(make_glb({"asset": {"version": "2.0"}, "scenes": [{}]}))
        with self.assertRaisesRegex(texture_worker.TextureError, "не найдены встроенные фототекстуры"):
            texture_worker.build_textured_model(self.mesh, self.workspace, self.openmvs,
                                                self.output, threads=1, runner=runner)
        self.assertEqual(len(calls), 2)
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.root.glob("spc-texture-*")), [])

    def test_cancel_request_between_openmvs_stages_keeps_output_absent(self):
        cancel = self.root / "cancel"
        calls, _, runner = self.runner_for(cancel_on_interface=cancel)
        with self.assertRaisesRegex(texture_worker.TextureCancelled, "остановлено пользователем"):
            texture_worker.build_textured_model(self.mesh, self.workspace, self.openmvs,
                                                self.output, threads=1, cancel_file=cancel, runner=runner)
        self.assertEqual(len(calls), 1)
        self.assertFalse(self.output.exists())

    def test_does_not_replace_existing_output(self):
        self.output.write_bytes(b"keep")
        calls, _, runner = self.runner_for()
        with self.assertRaisesRegex(texture_worker.TextureError, "уже существует"):
            texture_worker.build_textured_model(self.mesh, self.workspace, self.openmvs,
                                                self.output, threads=1, runner=runner)
        self.assertEqual(self.output.read_bytes(), b"keep")
        self.assertEqual(calls, [])

    def test_rejects_external_texture_sidecars(self):
        external = make_glb({
            "asset": {"version": "2.0"},
            "images": [{"uri": "textures/image0.png"}],
            "textures": [{"source": 0}],
            "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
        })
        glb = self.root / "external.glb"
        glb.write_bytes(external)
        with self.assertRaisesRegex(texture_worker.TextureError, "не является автономной"):
            texture_worker.inspect_glb(glb)

    def test_rejects_texture_path_escape_without_publishing_output(self):
        calls, _, runner = self.runner_for(make_external_glb("../outside.png"))
        with self.assertRaisesRegex(texture_worker.TextureError, "за пределами временной папки"):
            texture_worker.build_textured_model(self.mesh, self.workspace, self.openmvs,
                                                self.output, threads=1, runner=runner)
        self.assertEqual(len(calls), 2)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
