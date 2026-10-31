"""Develop Canon CR2 into local RGB copies with reconstruction-relevant EXIF."""
from __future__ import annotations

from pathlib import Path
import shutil


class RawImportError(Exception):
    pass


def read_metadata(path: Path) -> dict:
    try:
        import exifread
        with path.open("rb") as source:
            tags = exifread.process_file(source, details=False, strict=False)
    except Exception as exc:
        raise RawImportError(f"Не удалось прочитать данные камеры в {path.name}: {exc}") from exc

    def text(name: str) -> str | None:
        tag = tags.get(name)
        value = str(tag).strip().strip("\x00") if tag is not None else ""
        return value if value else None

    def integer(name: str) -> int | None:
        try:
            value = int(tags[name].values[0])
            return value if 0 < value <= 65535 else None
        except (AttributeError, KeyError, IndexError, TypeError, ValueError):
            return None

    def rational(name: str) -> tuple[int, int] | None:
        try:
            value = tags[name].values[0]
            numerator, denominator = int(value.num), int(value.den)
            if 0 < numerator <= 0xFFFFFFFF and 0 < denominator <= 0xFFFFFFFF:
                return numerator, denominator
        except (AttributeError, KeyError, IndexError, TypeError, ValueError, ZeroDivisionError):
            pass
        return None

    return {"make": text("Image Make"), "model": text("Image Model"),
            "focal_length": rational("EXIF FocalLength"),
            "focal_length_35mm": integer("EXIF FocalLengthIn35mmFilm"),
            "datetime": text("EXIF DateTimeOriginal"), "lens": text("EXIF LensModel")}


def encode_exif(metadata: dict) -> bytes:
    import piexif
    def ascii_bytes(value: str) -> bytes:
        return value.encode("ascii", errors="replace")[:1024]
    primary = {piexif.ImageIFD.Orientation: 1}
    exif = {}
    if metadata.get("make"):
        primary[piexif.ImageIFD.Make] = ascii_bytes(metadata["make"])
    if metadata.get("model"):
        primary[piexif.ImageIFD.Model] = ascii_bytes(metadata["model"])
    if metadata.get("focal_length"):
        exif[piexif.ExifIFD.FocalLength] = metadata["focal_length"]
    if metadata.get("focal_length_35mm"):
        exif[piexif.ExifIFD.FocalLengthIn35mmFilm] = metadata["focal_length_35mm"]
    if metadata.get("datetime"):
        exif[piexif.ExifIFD.DateTimeOriginal] = ascii_bytes(metadata["datetime"])
    if metadata.get("lens"):
        exif[piexif.ExifIFD.LensModel] = ascii_bytes(metadata["lens"])
    return piexif.dump({"0th": primary, "Exif": exif})


def convert_cr2(source: Path, destination: Path) -> dict:
    """Use full-size demosaiced pixels; original data is never modified."""
    try:
        import rawpy
        from PIL import Image
        metadata = read_metadata(source)
        # Opening through Python supports Unicode filenames on Windows.
        with source.open("rb") as stream, rawpy.imread(stream) as raw:
            # The default user_flip uses RAW orientation; EXIF is reset to 1.
            rgb = raw.postprocess(use_camera_wb=True, no_auto_bright=True, output_bps=8)
        image = Image.fromarray(rgb)
        image.save(destination, format="JPEG", quality=95, subsampling=0, exif=encode_exif(metadata))
        return metadata
    except RawImportError:
        raise
    except Exception as exc:
        raise RawImportError(f"Не удалось обработать CR2 {source.name}: {exc}") from exc


def prepare_images(paths: list[Path], images_root: Path, output_root: Path, check_cancel) -> tuple[Path, list[dict]]:
    """Create deterministic local working copies when at least one CR2 is present."""
    prepared = output_root / "prepared_images"
    prepared.mkdir()
    mapping = []
    for index, source in enumerate(paths):
        check_cancel()
        is_raw = source.suffix.lower() == ".cr2"
        filename = f"{index:06d}" + (".jpg" if is_raw else source.suffix.lower())
        target = prepared / filename
        metadata = None
        print(f"Подготовка фото {index + 1}/{len(paths)}: {source.name}", flush=True)
        if is_raw:
            metadata = convert_cr2(source, target)
            if not metadata.get("model") or not metadata.get("focal_length"):
                print("Предупреждение: модель камеры или фокусное расстояние отсутствуют в EXIF; "
                      "параметры камеры будут оцениваться автоматически.", flush=True)
        else:
            shutil.copyfile(source, target)
        mapping.append({"source": source.relative_to(images_root).as_posix(),
                        "prepared": filename, "format": "CR2" if is_raw else "original",
                        "camera_metadata": metadata})
    check_cancel()
    return prepared, mapping
