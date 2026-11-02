"""Run the built executable against real COLMAP/OpenMVS and synthetic views."""

import argparse
import hashlib
import json
from pathlib import Path
import queue
import subprocess
import threading

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
    mesh = root / "dense" / "dense-mesh.ply"
    model = root / "dense-textured.glb"
    mesh_result = subprocess.run(
        [str(executable), "--mesh-worker", "--input", str(dense / "dense.ply"), "--output", str(mesh), "--threads", "4"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
        check=False, timeout=600,
    )
    print(mesh_result.stdout, flush=True)
    if mesh_result.returncode != 0 or not mesh.is_file():
        raise RuntimeError(f"Packaged mesh worker failed with code {mesh_result.returncode}")
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
    check_dense(args.executable, args.images, args.output, args.openmvs)
    check_textured_glb(args.executable, args.output, args.openmvs)
    check_opencl_no_gpu(args.executable, args.images, args.output, args.colmap_cl)
    check_cr2(args.executable, args.cr2_sample, args.output)
