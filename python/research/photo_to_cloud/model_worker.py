"""Build a textured model from photographs with the existing CPU pipeline."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys
import tempfile
import time

from mesh_worker import inspect_mesh, publish_without_replacement
from reconstruct import (MAX_THREADS, OPENMVS_QUALITY, ReconstructionCancelled, ReconstructionError,
                         check_cancel, executable, image_paths, recommended_threads,
                         reconstruct, run_program, save_report)
from texture_worker import TextureCancelled, build_textured_model


def build_model(images: Path, output: Path, threads: int, openmvs_dir: Path | None,
                cancel_file: Path | None = None, *, quality_profile: str = "fast") -> None:
    images = images.expanduser().resolve()
    output = output.expanduser().resolve()
    if not 1 <= threads <= MAX_THREADS:
        raise ReconstructionError(f"Число потоков должно быть от 1 до {MAX_THREADS}.")
    if quality_profile not in OPENMVS_QUALITY:
        raise ReconstructionError("Неизвестный профиль качества.")
    if output.is_relative_to(images):
        raise ReconstructionError("Папка результата должна находиться вне папки фотографий.")
    if output.exists():
        raise ReconstructionError(f"Папка результата уже существует: {output}")
    image_paths(images)
    check_cancel(cancel_file)

    if openmvs_dir is None:
        if os.name != "nt":
            raise ReconstructionError("Укажите папку с программами OpenMVS.")
        from openmvs_install import default_destination, download_and_install

        openmvs_dir = download_and_install(default_destination(),
                                          lambda message: print(message, flush=True),
                                          check_cancel=lambda: check_cancel(cancel_file))
    openmvs_dir = openmvs_dir.expanduser().resolve()
    for name in ("InterfaceCOLMAP", "DensifyPointCloud", "TextureMesh"):
        executable(openmvs_dir, name)
    mesher = executable(openmvs_dir, "ReconstructMesh")

    print("Этап 1 из 3. Фотографии → плотное облако", flush=True)
    started = time.monotonic()
    reconstruct(images, output, "dense-cpu", threads, openmvs_dir=openmvs_dir,
                cancel_file=cancel_file, quality_profile=quality_profile)

    mesh_path = output / "model-mesh.ply"
    stage = "mesh"
    report = {"status": "failed", "stage": stage, "point_cloud": "dense.ply",
              "scale": "unknown", "quality_profile": quality_profile}
    try:
        print("Этап 2 из 3. Плотное облако → сетка", flush=True)
        with tempfile.TemporaryDirectory(prefix="mesh-", dir=output) as directory:
            candidate = Path(directory) / mesh_path.name
            # OpenMVS interprets dots in the full output path as an extension.
            # Keep the relative output name and the original scene working folder.
            run_program([str(mesher), "--input-file", str(output / "dense.mvs"),
                         "--output-file", str(candidate.relative_to(output)),
                         "--export-type", "ply", "--max-threads", str(threads)],
                        output, cancel_file)
            metadata = inspect_mesh(candidate)
            check_cancel(cancel_file)
            publish_without_replacement(candidate, mesh_path)
        report.update(mesh=mesh_path.name, mesh_faces=metadata["face_count"])
        stage = "texture"
        print("Этап 3 из 3. Сетка + фотографии → текстурированный GLB", flush=True)
        texture = build_textured_model(mesh_path, output / "dense_workspace", openmvs_dir,
                                       output / "model.glb", threads, cancel_file)
        report.update(status="succeeded", stage="complete", model="model.glb", texturing=texture)
    except (ReconstructionCancelled, TextureCancelled) as exc:
        report.update(status="cancelled", stage=stage, error=str(exc))
        raise
    except Exception as exc:
        report.update(stage=stage, error=str(exc))
        raise
    finally:
        # report.json records photo reconstruction separately and remains useful
        # even if a later stage is cancelled or fails.
        report["elapsed_seconds"] = round(time.monotonic() - started, 3)
        save_report(output / "model-report.json", report)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Построение текстурированной 3D-модели из фотографий")
    parser.add_argument("--images", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--threads", type=int, default=recommended_threads())
    parser.add_argument("--openmvs-dir", type=Path)
    parser.add_argument("--quality", choices=OPENMVS_QUALITY, default="fast")
    parser.add_argument("--cancel-file", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    try:
        build_model(args.images, args.output, args.threads, args.openmvs_dir, args.cancel_file,
                    quality_profile=args.quality)
    except (ReconstructionCancelled, TextureCancelled) as exc:
        print(str(exc), file=sys.stderr, flush=True)
        return 130
    except Exception as exc:
        print(f"Не удалось построить модель: {exc}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
