"""Optional, user-initiated installation of an unmodified OpenMVS release."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
import urllib.request
import zipfile


URL = "https://github.com/cdcseacave/openMVS/releases/download/v2.4.0/OpenMVS_Windows_x64.zip"
SHA256 = "0c31660c15c9ebc4c106873cf67564d9570d404aef7a6403451da1b6178b2167"
PREFIX = "vc17/x64/Release/"
MAX_ARCHIVE_BYTES = 100 * 1024 * 1024
MAX_EXTRACTED_BYTES = 600 * 1024 * 1024


class InstallError(Exception):
    pass


def _digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def default_destination() -> Path:
    base = Path(os.environ.get("LOCALAPPDATA", str(Path.home())))
    return base / "SpatialPhotoCloud" / "openmvs-2.4.0"


def _valid_installation(destination: Path, expected_sha: str) -> bool:
    try:
        root = destination.resolve()
        manifest = json.loads((root / ".installation.json").read_text(encoding="utf-8"))
        files = manifest["files"]
        if manifest["archive_sha256"] != expected_sha or not isinstance(files, dict):
            return False
        if not {"InterfaceCOLMAP.exe", "DensifyPointCloud.exe"}.issubset(files):
            return False
        for name, checksum in files.items():
            relative = PurePosixPath(name)
            if relative.is_absolute() or ".." in relative.parts or "\\" in name or ":" in name:
                return False
            target = (root / Path(*relative.parts)).resolve()
            if not target.is_relative_to(root) or not target.is_file() or _digest(target) != checksum:
                return False
        return True
    except (OSError, ValueError, KeyError, TypeError):
        return False


def install_archive(archive: Path, destination: Path, expected_sha: str = SHA256) -> Path:
    """Verify and extract into a new directory; never replace an existing one."""
    destination = destination.expanduser().resolve()
    if destination.exists():
        if _valid_installation(destination, expected_sha):
            return destination
        raise InstallError(f"Папка уже существует и не является проверенной установкой: {destination}")
    if _digest(archive) != expected_sha:
        raise InstallError("Контрольная сумма OpenMVS не совпадает; установка отменена.")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="openmvs-install-", dir=destination.parent) as temporary:
        staging = Path(temporary) / "files"
        staging.mkdir()
        try:
            with zipfile.ZipFile(archive) as package:
                entries = [entry for entry in package.infolist() if entry.filename.startswith(PREFIX) and not entry.is_dir()]
                if len(entries) > 1000 or sum(entry.file_size for entry in entries) > MAX_EXTRACTED_BYTES:
                    raise InstallError("Архив OpenMVS превышает допустимый размер.")
                for entry in entries:
                    name = entry.filename[len(PREFIX):]
                    relative = PurePosixPath(name)
                    if (not name or relative.is_absolute() or ".." in relative.parts or "\\" in name
                            or ":" in name or stat.S_ISLNK(entry.external_attr >> 16)):
                        raise InstallError("Архив OpenMVS содержит недопустимый путь.")
                    target = (staging / Path(*relative.parts)).resolve()
                    if not target.is_relative_to(staging.resolve()):
                        raise InstallError("Архив OpenMVS содержит недопустимый путь.")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with package.open(entry) as source, target.open("xb") as output:
                        shutil.copyfileobj(source, output, length=1024 * 1024)
        except (OSError, zipfile.BadZipFile) as exc:
            raise InstallError(f"Не удалось распаковать OpenMVS: {exc}") from exc
        if not (staging / "InterfaceCOLMAP.exe").is_file() or not (staging / "DensifyPointCloud.exe").is_file():
            raise InstallError("В архиве не найдены необходимые программы OpenMVS.")
        files = {path.relative_to(staging).as_posix(): _digest(path) for path in staging.rglob("*") if path.is_file()}
        (staging / ".installation.json").write_text(
            json.dumps({"archive_sha256": expected_sha, "files": files}, sort_keys=True) + "\n", encoding="utf-8")
        try:
            staging.rename(destination)
        except OSError as exc:
            raise InstallError(f"Не удалось сохранить OpenMVS: {exc}") from exc
    return destination


def download_and_install(destination: Path, progress=print) -> Path:
    if destination.exists():
        # The normal validation below accepts only an installation we recognise.
        if _valid_installation(destination, SHA256):
            return destination
        raise InstallError(f"Папка уже существует и не является проверенной установкой: {destination}")
    progress("Скачивание OpenMVS 2.4.0 с GitHub (около 22 МБ)…")
    with tempfile.TemporaryDirectory(prefix="openmvs-download-") as temporary:
        archive = Path(temporary) / "openmvs.zip"
        try:
            request = urllib.request.Request(URL, headers={"User-Agent": "SpatialPhotoCloud/0.1"})
            with urllib.request.urlopen(request, timeout=60) as response, archive.open("wb") as out:
                if not response.geturl().lower().startswith("https://"):
                    raise InstallError("Недопустимое перенаправление при загрузке OpenMVS.")
                size = 0
                while True:
                    chunk = response.read(1024 * 1024)
                    if not chunk:
                        break
                    size += len(chunk)
                    if size > MAX_ARCHIVE_BYTES:
                        raise InstallError("Архив OpenMVS превышает допустимый размер.")
                    out.write(chunk)
        except (OSError, ValueError) as exc:
            raise InstallError(f"Не удалось скачать OpenMVS: {exc}") from exc
        progress("Проверка архива и установка OpenMVS…")
        return install_archive(archive, destination)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Download a verified OpenMVS Windows release")
    parser.add_argument("--destination", type=Path, default=default_destination())
    args = parser.parse_args()
    print(download_and_install(args.destination), flush=True)
