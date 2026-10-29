"""Run the built executable against real COLMAP/OpenMVS and synthetic views."""

import argparse
import json
from pathlib import Path
import queue
import subprocess
import threading


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
            or not (output / "dense.ply").is_file() or not (output / "preview.html").is_file()):
        raise RuntimeError(f"invalid dense result: {report}")
    print(f"Packaged dense reconstruction: {report['registered_images']} cameras, "
          f"{report['point_count']} points, {report['preview_points']} preview points", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--images", required=True, type=Path)
    parser.add_argument("--openmvs", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    check_cancel(args.executable, args.images, args.output)
    check_dense(args.executable, args.images, args.output, args.openmvs)
