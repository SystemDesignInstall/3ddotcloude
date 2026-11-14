"""Run the built executable against real COLMAP/OpenMVS and synthetic views."""

import argparse
import hashlib
import json
from pathlib import Path
import queue
import struct
import subprocess
import threading

from test_lan_features import verify_lan_reconstruction
from texture_worker import inspect_glb


def check_cancel(executable: Path, images: Path, root: Path) -> None:
    cancel = root / "cancel-request"
    output = root / "cancelled"
    command = [str(executable), "--worker", "--images", str(images), "--output", str(output),
               "--mode", "sparse-cpu", "--threads", "4", "--cancel-file", str(cancel)]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding="utf-8", errors="replace")
    lines: queue.Queue = queue.Queue()
    def read() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            lines.put(line)
        lines.put(None)
    threading.Thread(target=read, daemon=True).start()
    try:
        while True:
            line = lines.get(timeout=120)
            if line is None:
                raise RuntimeError(f"worker exited before feature extraction (code {process.wait()})")
            print(line, end="", flush=True)
            if "1/4." in line:
                cancel.write_text("cancel\n", encoding="ascii")
                break
        code = process.wait(timeout=60)
        if code != 130:
            raise RuntimeError(f"cancelled worker returned {code}, expected 130")
        report = json.loads((output / "report.json").read_text(encoding="utf-8"))
        if report["status"] != "cancelled":
            raise RuntimeError(f"unexpected cancellation status: {report['status']}")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def check_dense(executable: Path, images: Path, root: Path, openmvs: Path) -> None:
    output = root / "dense"
    result = subprocess.run([str(executable), "--worker", "--images", str(images), "--output", str(output),
                             "--mode", "dense-cpu", "--openmvs-dir", str(openmvs), "--threads", "4"],
                            check=False, timeout=600)
    if result.returncode != 0:
        raise RuntimeError(f"dense worker failed with code {result.returncode}")
    report = json.loads((output / "report.json").read_text(encoding="utf-8"))
    if (report["status"] != "succeeded" or report["registered_images"] < 3 or report["point_count"] < 100
            or not (output / "dense.ply").is_file() or not (output / "preview.html").is_file()
            or not (output / "full_viewer.html").is_file()):
        raise RuntimeError(f"invalid dense result: {report}")
    print(f"Packaged dense reconstruction: {report['registered_images']} cameras, "
          f"{report['point_count']} points, {report['preview_points']} preview points", flush=True)


def check_textured_glb(executable: Path, root: Path, openmvs: Path) -> None:
    dense = root / "dense"
    mesh = root / "dense" / "texturing-test-mesh.ply"
    model = root / "dense-textured.glb"
    _write_camera_aligned_test_mesh(dense / "dense_workspace", mesh)
    texture_result = subprocess.run(
        [str(executable), "--texture-worker", "--mesh", str(mesh), "--workspace", str(dense / "dense_workspace"),
         "--openmvs-dir", str(openmvs), "--output", str(model), "--threads", "4"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
        check=False, timeout=900,
    )
    print(texture_result.stdout, flush=True)
    if texture_result.returncode != 0 or not model.is_file():
        raise RuntimeError(f"Packaged OpenMVS texturing failed with code {texture_result.returncode}")
    report = inspect_glb(model)
    print(f"Packaged textured GLB: {report['image_count']} images, "
          f"{report['texture_count']} textures, {report['material_count']} materials", flush=True)


def _read_first_colmap_pose(workspace: Path) -> tuple[list[list[float]], list[float]]:
    """Read the first registered camera pose from the COLMAP binary model."""
    sparse = workspace / "sparse"
    model = sparse / "0" if (sparse / "0" / "images.bin").is_file() else sparse
    image_file = model / "images.bin"
    with image_file.open("rb") as stream:
        count_data = stream.read(8)
        if len(count_data) != 8:
            raise RuntimeError(f"Invalid COLMAP image model: {image_file}")
        count, = struct.unpack("<Q", count_data)
        if count < 1:
            raise RuntimeError("COLMAP image model has no registered cameras")
        record = stream.read(struct.calcsize("<i7di"))
        if len(record) != struct.calcsize("<i7di"):
            raise RuntimeError("COLMAP image model has a truncated camera pose")
        _, qw, qx, qy, qz, tx, ty, tz, _ = struct.unpack("<i7di", record)
    rotation = [
        [1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)],
        [2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)],
        [2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)],
    ]
    translation = [tx, ty, tz]
    center = [-sum(rotation[row][axis] * translation[row] for row in range(3)) for axis in range(3)]
    return rotation, center


def _write_camera_aligned_test_mesh(workspace: Path, output: Path) -> None:
    """Place a small plane in a real reconstructed camera's view for texturing QA."""
    rotation, center = _read_first_colmap_pose(workspace)
    half_width, half_height, depth = 1.2, 0.9, 5.5
    camera_vertices = [
        (-half_width, -half_height, depth),
        (half_width, -half_height, depth),
        (half_width, half_height, depth),
        (-half_width, half_height, depth),
    ]
    # COLMAP qvec/tvec map world coordinates to camera coordinates. Transform
    # the fixture plane back into the same world frame as the dense workspace.
    vertices = []
    for point in camera_vertices:
        vertices.append([
            center[axis] + sum(rotation[row][axis] * point[row] for row in range(3))
            for axis in range(3)
        ])
    with output.open("x", encoding="ascii", newline="\n") as stream:
        stream.write(
            "ply\nformat ascii 1.0\nelement vertex 4\n"
            "property float x\nproperty float y\nproperty float z\n"
            "element face 2\nproperty list uchar int vertex_indices\nend_header\n"
        )
        for vertex in vertices:
            stream.write("{:.9g} {:.9g} {:.9g}\n".format(*vertex))
        # Reverse the camera-plane winding so both faces point toward camera 0.
        stream.write("3 0 2 1\n3 0 3 2\n")


def check_opencl_no_gpu(executable: Path, images: Path, root: Path, colmap_cl: Path) -> None:
    output = root / "opencl-without-gpu"
    result = subprocess.run([str(executable), "--worker", "--images", str(images), "--output", str(output),
                             "--mode", "dense-opencl", "--colmap", str(colmap_cl), "--threads", "4"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", check=False, timeout=120)
    print(result.stdout, flush=True)
    if result.returncode != 1 or output.exists() or "не найдено" not in result.stdout:
        raise RuntimeError("AMD mode must reject a CPU-only OpenCL system without creating a result")


def check_mesh_worker_entrypoint(executable: Path, root: Path) -> None:
    source = root / "mesh-worker-no-normals.ply"
    output = root / "mesh-worker-output.ply"
    source.write_bytes(
        b"ply\nformat ascii 1.0\nelement vertex 3\n"
        b"property float x\nproperty float y\nproperty float z\n"
        b"end_header\n0 0 0\n1 0 0\n0 1 0\n"
    )
    result = subprocess.run(
        [str(executable), "--mesh-worker", "--input", str(source), "--output", str(output), "--threads", "1"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", check=False,
    )
    print(result.stdout, flush=True)
    if result.returncode != 1 or "нет нормалей" not in result.stdout or output.exists():
        raise RuntimeError("Packaged Poisson worker did not validate its input safely")


def check_texture_worker_entrypoint(executable: Path, openmvs: Path, root: Path) -> None:
    mesh = root / "texture-worker-mesh.ply"
    output = root / "texture-worker-output.glb"
    mesh.write_bytes(
        b"ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
        b"element face 1\nproperty list uchar int vertex_indices\nend_header\n"
        b"0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n"
    )
    result = subprocess.run(
        [str(executable), "--texture-worker", "--mesh", str(mesh), "--workspace", str(root / "missing-workspace"),
         "--openmvs-dir", str(openmvs), "--output", str(output), "--threads", "1"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", check=False,
    )
    print(result.stdout, flush=True)
    if result.returncode != 1 or "Папка реконструкции не найдена" not in result.stdout or output.exists():
        raise RuntimeError("Packaged texture worker did not validate workspace input safely")


def check_cr2(executable: Path, sample: Path, root: Path) -> None:
    from PIL import Image
    import pycolmap
    from raw_import import read_metadata

    before = hashlib.sha256(sample.read_bytes()).hexdigest()
    target = root / "developed.jpg"
    result = subprocess.run([str(executable), "--raw-conversion-test", str(sample), str(target)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", check=False, timeout=180)
    print(result.stdout, flush=True)
    if result.returncode != 0 or not target.is_file() or before != hashlib.sha256(sample.read_bytes()).hexdigest():
        raise RuntimeError("Packaged CR2 conversion failed or modified the original")
    source, converted = read_metadata(sample), read_metadata(target)
    if not source["model"] or not source["focal_length"]:
        raise RuntimeError(f"Test CR2 is missing expected camera metadata: {source}")
    if (source["model"], source["make"], source["focal_length"]) != (
            converted["model"], converted["make"], converted["focal_length"]):
        raise RuntimeError(f"CR2 metadata was not preserved: {source} -> {converted}")
    with Image.open(target) as image:
        if min(image.size) < 1000 or image.getexif().get(274) != 1:
            raise RuntimeError(f"Unexpected CR2 image size or orientation: {image.size}")
    bitmap = pycolmap.Bitmap.read(target, True)
    if bitmap is None or not bitmap.exif_camera_model() or bitmap.exif_focal_length() is None:
        raise RuntimeError("COLMAP could not read the converted image's camera and focal length")
    print(f"Packaged CR2 conversion: {source['model']}, focal {source['focal_length']}; COLMAP reads EXIF", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--images", required=True, type=Path)
    parser.add_argument("--openmvs", required=True, type=Path)
    parser.add_argument("--colmap-cl", required=True, type=Path)
    parser.add_argument("--cr2-sample", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    check_mesh_worker_entrypoint(args.executable, args.output)
    check_texture_worker_entrypoint(args.executable, args.openmvs, args.output)
    check_cancel(args.executable, args.images, args.output)
    lan_root = args.output / 'lan'
    lan_root.mkdir()
    verify_lan_reconstruction([str(args.executable)], args.images, lan_root)
    check_dense(args.executable, args.images, args.output, args.openmvs)
    check_textured_glb(args.executable, args.output, args.openmvs)
    check_opencl_no_gpu(args.executable, args.images, args.output, args.colmap_cl)
    check_cr2(args.executable, args.cr2_sample, args.output)
