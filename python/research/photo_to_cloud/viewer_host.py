"""Serve exactly one user-selected cloud to the local full-resolution viewer."""
from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import threading
from urllib.parse import urlsplit
import webbrowser


class ViewerHost:
    """A per-window loopback server; paths never expose a directory listing."""

    def __init__(self, cloud: Path) -> None:
        cloud = cloud.expanduser().resolve(strict=True)
        if not cloud.is_file() or cloud.suffix.casefold() != ".ply":
            raise ValueError("Выберите существующий файл PLY.")
        page = Path(__file__).with_name("full_viewer.html").read_bytes()
        token = secrets.token_urlsafe(32)
        prefix = f"/{token}/"

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
