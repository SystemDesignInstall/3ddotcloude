"""User-initiated, separate installation of the upstream Windows COLMAP-CL release."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
import urllib.request
import zipfile

URL = "https://github.com/openphotogrammetry/colmap-cl/releases/download/v1.5/COLMAP-CL-1.5-windows.zip"
# Measured from the upstream archive during research; not a publisher-provided checksum.
SHA256 = "0439d39d1231b7a6cd3b521f9c5ca327d9bec1c88084b38af36c942256ec1750"
MAX_ARCHIVE_BYTES = 500 * 1024 * 1024
MAX_EXTRACTED_BYTES = 2 * 1024 * 1024 * 1024


class InstallError(Exception):
    pass


def digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def default_destination() -> Path:
    base = Path(os.environ.get("LOCALAPPDATA", str(Path.home())))
    return base / "SpatialPhotoCloud" / "colmap-cl-1.5"


def valid_installation(root: Path, expected_sha: str = SHA256) -> bool:
    try:
        root = root.resolve()
        manifest = json.loads((root / ".installation.json").read_text(encoding="utf-8"))
        files = manifest["files"]
        if manifest["archive_sha256"] != expected_sha or not isinstance(files, dict):
            return False
        if "bin/colmap.exe" not in files or not any(name.startswith("lib/") for name in files):
            return False
        for name, checksum in files.items():
            relative = PurePosixPath(name)
            if relative.is_absolute() or ".." in relative.parts or "\\" in name or ":" in name:
                return False
            path = (root / Path(*relative.parts)).resolve()
            if not path.is_relative_to(root) or not path.is_file() or digest(path) != checksum:
                return False
        return True
    except (OSError, ValueError, KeyError, TypeError):
        return False


def install_archive(archive: Path, destination: Path, expected_sha: str = SHA256) -> Path:
    destination = destination.expanduser().resolve()
    if destination.exists():
        if valid_installation(destination, expected_sha):
            return destination
        raise InstallError(f"Папка уже существует и не является проверенной установкой: {destination}")
    if digest(archive) != expected_sha:
        raise InstallError("Контрольная сумма COLMAP-CL не совпадает; установка отменена.")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="colmap-cl-install-", dir=destination.parent) as temporary:
        staging = Path(temporary) / "files"
        staging.mkdir()
        try:
            with zipfile.ZipFile(archive) as package:
                entries = [entry for entry in package.infolist() if not entry.is_dir()]
                if len(entries) > 10000 or sum(entry.file_size for entry in entries) > MAX_EXTRACTED_BYTES:
                    raise InstallError("Архив COLMAP-CL превышает допустимый размер.")
                for entry in entries:
                    name = entry.filename
                    relative = PurePosixPath(name)
                    if (not name or relative.is_absolute() or ".." in relative.parts or "\\" in name
                            or ":" in name or stat.S_ISLNK(entry.external_attr >> 16)):
                        raise InstallError("Архив COLMAP-CL содержит недопустимый путь.")
                    target = (staging / Path(*relative.parts)).resolve()
                    if not target.is_relative_to(staging.resolve()):
                        raise InstallError("Архив COLMAP-CL содержит недопустимый путь.")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with package.open(entry) as source, target.open("xb") as output:
                        shutil.copyfileobj(source, output, length=1024 * 1024)
        except (OSError, zipfile.BadZipFile) as exc:
            raise InstallError(f"Не удалось распаковать COLMAP-CL: {exc}") from exc
        if not (staging / "bin" / "colmap.exe").is_file() or not (staging / "lib").is_dir():
            raise InstallError("В архиве не найдены bin/colmap.exe и каталог lib.")
        files = {path.relative_to(staging).as_posix(): digest(path)
                 for path in staging.rglob("*") if path.is_file()}
        (staging / ".installation.json").write_text(
            json.dumps({"archive_sha256": expected_sha, "files": files}, sort_keys=True) + "\n", encoding="utf-8")
        try:
            staging.rename(destination)
        except OSError as exc:
            raise InstallError(f"Не удалось сохранить COLMAP-CL: {exc}") from exc
    return destination


def download_and_install(destination: Path, progress=print) -> Path:
    if destination.exists():
        if valid_installation(destination):
            return destination
        raise InstallError(f"Папка уже существует и не является проверенной установкой: {destination}")
    progress("Скачивание внешнего COLMAP-CL 1.5 из официального релиза GitHub…")
    with tempfile.TemporaryDirectory(prefix="colmap-cl-download-") as temporary:
        archive = Path(temporary) / "colmap-cl.zip"
        try:
            request = urllib.request.Request(URL, headers={"User-Agent": "SpatialPhotoCloud/0.1"})
            with urllib.request.urlopen(request, timeout=60) as response, archive.open("wb") as output:
                if not response.geturl().lower().startswith("https://"):
                    raise InstallError("Недопустимое перенаправление при загрузке COLMAP-CL.")
                size = 0
                while True:
                    block = response.read(1024 * 1024)
                    if not block:
                        break
                    size += len(block)
                    if size > MAX_ARCHIVE_BYTES:
                        raise InstallError("Архив COLMAP-CL превышает допустимый размер.")
                    output.write(block)
        except (OSError, ValueError) as exc:
            raise InstallError(f"Не удалось скачать COLMAP-CL: {exc}") from exc
        progress("Проверка архива и установка COLMAP-CL…")
        return install_archive(archive, destination)


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Download an external COLMAP-CL Windows release")
    parser.add_argument("--destination", type=Path, default=default_destination())
    arguments = parser.parse_args()
    print(download_and_install(arguments.destination), flush=True)
