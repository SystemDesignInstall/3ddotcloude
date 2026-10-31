"""Serve exactly one user-selected cloud to the local full-resolution viewer."""
from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import shutil
import stat
import subprocess
import tempfile
import threading
from urllib.parse import urlsplit
import webbrowser

_SAVE_LOCK = threading.Lock()
_MAX_HEADER = 1024 * 1024


class CloudChangedError(Exception):
    """The source was changed after this viewer session opened it."""


class InvalidCloudError(Exception):
    """A replacement does not match the selected cloud's structure."""


class _RequestBody:
    def __init__(self, stream, length: int) -> None:
        self.stream = stream
        self.remaining = length

    def read(self, size: int) -> bytes:
        data = self.stream.read(min(size, self.remaining))
        self.remaining -= len(data)
        return data

    def drain(self) -> None:
        while self.remaining:
            if not self.read(min(1024 * 1024, self.remaining)):
                break


def _fingerprint(path: Path) -> tuple[int, int, int, int]:
    info = path.stat()
    return info.st_size, info.st_mtime_ns, info.st_dev, info.st_ino


def _header(path: Path) -> tuple[tuple[bytes, ...], int, int, bytes]:
    declarations = []
    vertex_count = None
    file_format = None
    with path.open("rb") as stream:
        first = stream.readline(_MAX_HEADER + 1)
        if first.strip() != b"ply":
            raise InvalidCloudError("В файле отсутствует заголовок PLY.")
        while stream.tell() <= _MAX_HEADER:
            line = stream.readline(_MAX_HEADER + 1)
            if not line:
                break
            words = line.strip().split()
            if words and words[0] in (b"format", b"element", b"property"):
                declarations.append(b" ".join(words))
                if words[0] == b"format" and len(words) >= 2:
                    file_format = words[1]
                if words[0] == b"element" and len(words) == 3 and words[1] == b"vertex":
                    try:
                        vertex_count = int(words[2])
                    except ValueError as exc:
                        raise InvalidCloudError("Некорректное число точек в PLY.") from exc
            if words and words[0] == b"end_header":
                if file_format not in (b"ascii", b"binary_little_endian", b"binary_big_endian") or not vertex_count or vertex_count < 0:
                    raise InvalidCloudError("Неподдерживаемый заголовок PLY.")
                return tuple(declarations), stream.tell(), vertex_count, file_format
    raise InvalidCloudError("Заголовок PLY неполный или слишком большой.")


def _check_replacement(source: Path, candidate: Path) -> None:
    old, old_offset, count, file_format = _header(source)
    new, new_offset, _, _ = _header(candidate)
    if old != new:
        raise InvalidCloudError("Структура нового PLY не совпадает с исходным файлом.")
    if file_format != b"ascii":
        if source.stat().st_size - old_offset != candidate.stat().st_size - new_offset:
            raise InvalidCloudError("Данные нового PLY имеют неверный размер.")
    else:
        # The exporter emits one newline-terminated line per vertex.
        with candidate.open("rb") as stream:
            stream.seek(new_offset)
            lines = 0
            while lines < count:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    raise InvalidCloudError("Данные точек в новом PLY неполные.")
                lines += chunk.count(b"\n")


class CloudSaveSession:
    """Replace one chosen file only after a complete, validated write and backup."""

    def __init__(self, cloud: Path) -> None:
        self.cloud = cloud
        self.snapshot = _fingerprint(cloud)
        self.backup: Path | None = None
        self.backup_snapshot: tuple[int, int, int, int] | None = None

    def _ensure_unchanged(self) -> None:
        if _fingerprint(self.cloud) != self.snapshot:
            raise CloudChangedError("PLY изменился вне программы. Откройте файл заново перед сохранением.")

    def save(self, stream, length: int) -> Path:
        if length <= 0:
            raise InvalidCloudError("Новый PLY пуст.")
        temporary: Path | None = None
        with _SAVE_LOCK:
            try:
                self._ensure_unchanged()
                if self.backup is not None:
                    try:
                        if _fingerprint(self.backup) != self.backup_snapshot:
                            self.backup = None
                    except OSError:
                        self.backup = None
                # Reserve room for the transformed file and the first backup.
                required = length + (self.snapshot[0] if self.backup is None else 0)
                if shutil.disk_usage(self.cloud.parent).free < required:
                    raise OSError("Недостаточно свободного места для сохранения и резервной копии.")
                with tempfile.NamedTemporaryFile(mode="wb", prefix=".spatial-save-", suffix=".tmp",
                                                 dir=self.cloud.parent, delete=False) as target:
                    temporary = Path(target.name)
                    remaining = length
                    while remaining:
                        chunk = stream.read(min(1024 * 1024, remaining))
                        if not chunk:
                            raise InvalidCloudError("Сохранение прервано: получены не все данные PLY.")
                        target.write(chunk)
                        remaining -= len(chunk)
                    target.flush()
                    os.fsync(target.fileno())
                _check_replacement(self.cloud, temporary)
                self._ensure_unchanged()
                if self.backup is None:
                    backup_dir = self.cloud.parent / ".SpatialPhotoCloud-backups"
                    backup_dir.mkdir(exist_ok=True)
                    backup = backup_dir / (self.cloud.stem + ".before-alignment." + secrets.token_hex(8) + ".ply")
                    try:
                        with self.cloud.open("rb") as source, backup.open("xb") as target:
                            shutil.copyfileobj(source, target, length=1024 * 1024)
                            target.flush()
                            os.fsync(target.fileno())
                        shutil.copystat(self.cloud, backup)
                    except Exception:
                        backup.unlink(missing_ok=True)
                        raise
                    self.backup = backup
                    self.backup_snapshot = _fingerprint(backup)
                self._ensure_unchanged()
                os.chmod(temporary, stat.S_IMODE(self.cloud.stat().st_mode))
                os.replace(temporary, self.cloud)
                temporary = None
                self.snapshot = _fingerprint(self.cloud)
                return self.backup
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)


class ViewerHost:
    """A per-window loopback server; paths never expose a directory listing."""

    def __init__(self, cloud: Path) -> None:
        cloud = cloud.expanduser().resolve(strict=True)
        if not cloud.is_file() or cloud.suffix.casefold() != ".ply":
            raise ValueError("Выберите существующий файл PLY.")
        page = Path(__file__).with_name("full_viewer.html").read_bytes()
        token = secrets.token_urlsafe(32)
        prefix = f"/{token}/"
        saver = CloudSaveSession(cloud)

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, _format, *_args):
                pass  # Do not log the private session URL or file name.

            def send_content(self, code: int, content_type: str, length: int) -> None:
                self.send_response(code)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(length))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.send_header("Referrer-Policy", "no-referrer")
                self.send_header("Cross-Origin-Resource-Policy", "same-origin")
                self.send_header("Content-Security-Policy", "default-src 'self' blob:; "
                                 "script-src 'unsafe-inline' blob:; style-src 'unsafe-inline'; "
                                 "img-src 'self' data: blob:; worker-src blob:; "
                                 "object-src 'none'; base-uri 'none'; frame-ancestors 'none'")
                self.end_headers()

            def do_GET(self) -> None:
                # Bind to the literal loopback host as well as a private, random path.
                host = self.headers.get("Host", "")
                if host != f"127.0.0.1:{self.server.server_port}":
                    self.send_error(404)
                    return
                path = urlsplit(self.path).path
                if path == prefix:
                    self.send_content(200, "text/html; charset=utf-8", len(page))
                    self.wfile.write(page)
                elif path == prefix + "meta.json":
                    body = json.dumps({"name": cloud.name}, ensure_ascii=False).encode("utf-8")
                    self.send_content(200, "application/json; charset=utf-8", len(body))
                    self.wfile.write(body)
                elif path == prefix + "cloud":
                    try:
                        with cloud.open("rb") as source:
                            size = os.fstat(source.fileno()).st_size
                            self.send_content(200, "application/octet-stream", size)
                            shutil.copyfileobj(source, self.wfile, length=1024 * 1024)
                    except (FileNotFoundError, PermissionError):
                        self.send_error(404)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                else:
                    self.send_error(404)

            def do_POST(self) -> None:
                expected_host = f"127.0.0.1:{self.server.server_port}"
                if (self.headers.get("Host") != expected_host or
                    self.headers.get("Origin") != "http://" + expected_host or
                    self.headers.get("X-Spatial-Action") != "save-current-ply" or
                    self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower() != "application/octet-stream" or
                    urlsplit(self.path).path != prefix + "save"):
                    self.send_error(403)
                    return
                try:
                    length = int(self.headers.get("Content-Length", ""))
                    if length <= 0 or self.headers.get("Transfer-Encoding"):
                        raise ValueError
                except ValueError:
                    self.send_error(400, "Invalid content length")
                    return
                reader = _RequestBody(self.rfile, length)
                try:
                    self.connection.settimeout(30)
                    backup = saver.save(reader, length)
                    payload = {"ok": True, "backup": str(backup.relative_to(cloud.parent))}
                    code = 200
                except CloudChangedError as exc:
                    payload, code = {"error": str(exc)}, 409
                except (InvalidCloudError, OSError) as exc:
                    payload, code = {"error": str(exc)}, 400
                # A rejected upload may still be in flight. Consume its body
                # before replying so browsers can receive the error instead
                # of observing a TCP reset.
                try:
                    reader.drain()
                except OSError:
                    return
                body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
                self.send_content(code, "application/json; charset=utf-8", len(body))
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": 0.1}, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}{prefix}?app=1"
        self.closed = False

    def close(self) -> None:
        if not self.closed:
            self.closed = True
            self.server.shutdown()
            self.server.server_close()


def browser_app() -> Path | None:
    """Prefer an installed Windows Edge/Chrome for a dedicated app window."""
    if os.name != "nt":
        return None
    candidates = []
    for name in ("PROGRAMFILES(X86)", "PROGRAMFILES", "LOCALAPPDATA"):
        root = os.environ.get(name)
        if root:
            candidates.extend((Path(root) / "Microsoft/Edge/Application/msedge.exe",
                               Path(root) / "Google/Chrome/Application/chrome.exe"))
    for name in ("msedge.exe", "chrome.exe"):
        candidate = shutil.which(name)
        if candidate:
            candidates.append(Path(candidate))
    return next((path for path in candidates if path.is_file()), None)


def open_window(host: ViewerHost) -> bool:
    browser = browser_app()
    if browser:
        # PyInstaller alters Windows DLL search paths; restore them while
        # starting an external browser, then restore the application's paths.
        from colmap_cl_backend import external_dll_search
        with external_dll_search():
            subprocess.Popen([str(browser), "--app=" + host.url],
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return True
    return bool(webbrowser.open(host.url))
