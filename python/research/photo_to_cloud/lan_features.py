"""Parallel CPU SIFT extraction for one Photo Cloud project over the LAN."""

from __future__ import annotations

import argparse
from collections import deque
from contextlib import closing
from dataclasses import dataclass
import datetime
import hashlib
import hmac
from http.client import HTTPException, HTTPSConnection
from http.server import BaseHTTPRequestHandler, HTTPServer
import io
import os
from pathlib import Path
from queue import Empty, SimpleQueue
import re
import secrets
import select
import socket
import sqlite3
import ssl
import struct
import sys
from tempfile import TemporaryDirectory
from threading import Event, Lock, Thread
import time
from urllib.parse import urlsplit
from zipfile import BadZipFile, ZIP_STORED, ZipFile


PYCOLMAP_VERSION = "4.2.1"
PAIRING_ENV = "SPATIAL_PHOTO_NODE"
PORT = 8462
BATCH_SIZE = 16
MAX_UPLOAD_BYTES = 1024 ** 3
MAX_FEATURES = 16384
KEYPOINT_COLS, DESCRIPTOR_COLS = 6, 128
FEATURE_BYTES = KEYPOINT_COLS * 4 + DESCRIPTOR_COLS
MAX_RESULT = BATCH_SIZE * (4 + MAX_FEATURES * FEATURE_BYTES)
CHUNK_SIZE = 256 * 1024
FEATURE_PATH = f"/v1/sift/{PYCOLMAP_VERSION}"
HEALTH_PATH = f"{FEATURE_PATH}/health"
ROW_HEADER = struct.Struct("<I")
IMAGE_NAME = re.compile(r"[0-9]{2}\.(?:jpeg?|png|tiff?)")


class NodeError(Exception):
    pass


@dataclass(frozen=True)
class Pairing:
    host: str
    port: int
    fingerprint: str
    token: str

    @classmethod
    def parse(cls, code: str) -> Pairing:
        try:
            url = urlsplit(f"//{code.strip()}")
            fingerprint, token = url.path.removeprefix("/").split(".")
            if (not url.hostname or url.port in (None, 0) or url.username is not None
                    or url.query or url.fragment
                    or not re.fullmatch(r"[0-9a-f]{64}", fingerprint)
                    or not re.fullmatch(r"[0-9a-f]{32}", token)):
                raise ValueError
            return cls(url.hostname, url.port, fingerprint, token)
        except ValueError:
            raise NodeError("Ожидается код второго ПК в формате IP:порт/отпечаток.ключ.") from None


def load_pycolmap():
    import pycolmap

    if pycolmap.__version__ != PYCOLMAP_VERSION:
        raise NodeError(f"Нужен PyCOLMAP {PYCOLMAP_VERSION}; установлен {pycolmap.__version__}.")
    return pycolmap


def extract(pycolmap, database: Path, images: Path, names: list[str], threads: int, token) -> None:
    options = pycolmap.FeatureExtractionOptions(
        num_threads=threads, max_image_size=2000, use_gpu=False,
    )
    pycolmap.extract_features(database, images, image_names=names, extraction_options=options,
                             device=pycolmap.Device.cpu, cancellation_token=token)


def copy_bytes(source, target, size: int, check=lambda: None) -> None:
    while size:
        check()
        block = source.read(min(size, CHUNK_SIZE))
        if not block:
            raise NodeError("Передача завершилась раньше ожидаемого.")
        target.write(block)
        size -= len(block)


def make_upload(path: Path, images: Path, names: list[str], check) -> None:
    with ZipFile(path, "w", compression=ZIP_STORED) as archive:
        for index, name in enumerate(names):
            source = images / name
            size = source.stat().st_size
            if archive.fp.tell() + size > MAX_UPLOAD_BYTES - 4096:
                raise NodeError("Пакет фотографий превышает 1 ГиБ.")
            with source.open("rb") as stream, archive.open(f"{index:02}{source.suffix.lower()}", "w") as entry:
                copy_bytes(stream, entry, size, check)


def unpack_upload(path: Path, destination: Path) -> list[str]:
    with ZipFile(path) as archive:
        entries = archive.infolist()
        if (not 1 <= len(entries) <= BATCH_SIZE
                or sum(entry.file_size for entry in entries) > MAX_UPLOAD_BYTES):
            raise NodeError("Неверный размер пакета.")
        for index, entry in enumerate(entries):
            if (not IMAGE_NAME.fullmatch(entry.filename) or entry.filename[:2] != f"{index:02}"
                    or entry.compress_type != ZIP_STORED or entry.file_size != entry.compress_size):
                raise NodeError("Неверный формат пакета фотографий.")
            with archive.open(entry) as source, (destination / entry.filename).open("xb") as target:
                copy_bytes(source, target, entry.file_size)
    return [entry.filename for entry in entries]


def write_features(database: Path, names: list[str], output: Path) -> None:
    # Read only the SIFT matrices. The main PC owns image IDs and calibration.
    with closing(sqlite3.connect(database)) as db, output.open("xb") as stream:
        for name in names:
            row = db.execute(
                """SELECT k.rows, k.cols, k.data, d.rows, d.cols, d.type, d.data
                   FROM images i JOIN keypoints k USING (image_id)
                   JOIN descriptors d USING (image_id) WHERE i.name = ?""",
                (name,),
            ).fetchone()
            if row is None:
                raise NodeError("PyCOLMAP не вернул признаки для одного из кадров.")
            count, key_cols, keypoints, desc_count, desc_cols, desc_type, descriptors = row
            keypoints, descriptors = keypoints or b"", descriptors or b""
            if (not 0 <= count <= MAX_FEATURES or desc_count != count or desc_type != 0
                    or key_cols != KEYPOINT_COLS or desc_cols != DESCRIPTOR_COLS
                    or len(keypoints) != count * KEYPOINT_COLS * 4
                    or len(descriptors) != count * DESCRIPTOR_COLS):
                raise NodeError("PyCOLMAP вернул несовместимые SIFT-данные.")
            stream.write(ROW_HEADER.pack(count))
            stream.write(keypoints)
            stream.write(descriptors)


def read_exact(stream, size: int) -> bytes:
    block = stream.read(size)
    if len(block) != size:
        raise NodeError("Неполный пакет признаков.")
    return block


def import_features(database: Path, names: list[str], result: Path, check) -> None:
    # A single transaction makes a malformed or interrupted packet safe to retry.
    import numpy as np

    with closing(sqlite3.connect(database)) as db, db, result.open("rb") as stream:
        for name in names:
            check()
            count, = ROW_HEADER.unpack(read_exact(stream, ROW_HEADER.size))
            if count > MAX_FEATURES:
                raise NodeError("Слишком много признаков в пакете.")
            keypoints = read_exact(stream, count * KEYPOINT_COLS * 4)
            descriptors = read_exact(stream, count * DESCRIPTOR_COLS)
            if not np.isfinite(np.frombuffer(keypoints, dtype="<f4")).all():
                raise NodeError("Пакет содержит некорректные координаты признаков.")
            image = db.execute("SELECT image_id FROM images WHERE name = ?", (name,)).fetchone()
            if image is None:
                raise NodeError("Кадр отсутствует в базе проекта.")
            db.execute("INSERT INTO keypoints (image_id, rows, cols, data) VALUES (?, ?, ?, ?)",
                       (image[0], count, KEYPOINT_COLS, keypoints or None))
            db.execute("INSERT INTO descriptors (image_id, type, rows, cols, data) VALUES (?, 0, ?, ?, ?)",
                       (image[0], count, DESCRIPTOR_COLS, descriptors or None))
        if stream.read(1):
            raise NodeError("В пакете признаков остались лишние данные.")


class NodeClient:
    def __init__(self, pairing: Pairing) -> None:
        self.pairing = pairing
        self.stopped = Event()
        self.lock = Lock()
        self.connection_socket = None

    def stop(self) -> None:
        self.stopped.set()
        with self.lock:
            if self.connection_socket is not None:
                try:
                    self.connection_socket.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def check(self) -> None:
        if self.stopped.is_set():
            raise NodeError("Обмен со вторым ПК остановлен.")

    def request(self, path: str, upload: Path | None, result) -> None:
        self.check()
        # The printed pairing code pins the node's generated certificate.
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        with closing(HTTPSConnection(self.pairing.host, self.pairing.port, timeout=4,
                                     context=context)) as connection:
            connection.connect()
            transport = connection.sock
            with self.lock:
                self.connection_socket = transport
            try:
                self.check()
                fingerprint = hashlib.sha256(transport.getpeercert(binary_form=True)).hexdigest()
                if not hmac.compare_digest(fingerprint, self.pairing.fingerprint):
                    raise NodeError("Отпечаток сертификата второго ПК не совпал.")
                headers = {"Authorization": f"Bearer {self.pairing.token}"}
                if upload is None:
                    connection.request("GET", path, headers=headers)
                else:
                    headers["Content-Length"] = str(upload.stat().st_size)
                    # Each request is bounded; a stalled batch is recalculated locally.
                    transport.settimeout(90)
                    with upload.open("rb") as body:
                        connection.request("POST", path, body=body, headers=headers)
                with connection.getresponse() as response:
                    if response.status != 200:
                        raise NodeError(f"Второй ПК отклонил запрос (HTTP {response.status}).")
                    length = int(response.getheader("Content-Length", "-1"))
                    if not 0 <= length <= (MAX_RESULT if upload is not None else 64):
                        raise NodeError("Неверный размер ответа второго ПК.")
                    copy_bytes(response, result, length, self.check)
            finally:
                with self.lock:
                    self.connection_socket = None


def extract_distributed(pycolmap, database: Path, images: Path, names: list[str], threads: int,
                        token, pairing: Pairing, check_cancel, log) -> dict:
    started = time.monotonic()
    stats = {"status": "local_fallback", "local_images": len(names), "remote_images": 0,
             "remote_batches": 0, "fallback_batches": 0}
    client = NodeClient(pairing)
    try:
        health = io.BytesIO()
        client.request(HEALTH_PATH, None, health)
        remote_threads = int(health.getvalue())
        if not 1 <= remote_threads <= 64:
            raise NodeError("Второй ПК сообщил неверное число потоков.")
    except (OSError, HTTPException, ValueError, NodeError) as exc:
        check_cancel()
        log(f"Второй ПК недоступен ({exc}). Все признаки считаются на основном ПК.")
        extract(pycolmap, database, images, names, threads, token)
        stats["seconds"] = round(time.monotonic() - started, 3)
        return stats

    log(f"Второй ПК подключён ({remote_threads} потоков). Распределяем фотографии.")
    # Import calibration once, with the original relative paths and EXIF.
    with pycolmap.Database.open(database):
        pass
    pycolmap.import_images(database, images, image_names=names)
    check_cancel()
    batch_size = min(BATCH_SIZE, len(names) // 2)
    batches = deque((index, names[offset:offset + batch_size])
                    for index, offset in enumerate(range(0, len(names), batch_size)))
    remote_batch = batches.popleft()
    local_batch = batches.popleft()
    lock = Lock()
    completed = SimpleQueue()
    stats["local_images"] = 0

    def take_batch():
        with lock:
            return batches.popleft() if batches else None

    with TemporaryDirectory(prefix="lan-features-", dir=database.parent) as folder:
        root = Path(folder)

        def run_remote() -> None:
            batch = remote_batch
            while batch is not None:
                index, batch_names = batch
                result = root / f"{index}.sift"
                try:
                    with TemporaryDirectory(dir=root) as upload_dir:
                        upload = Path(upload_dir) / "images.zip"
                        make_upload(upload, images, batch_names, client.check)
                        with result.open("xb") as stream:
                            client.request(FEATURE_PATH, upload, stream)
                except Exception as exc:
                    completed.put((batch_names, None, str(exc)))
                    return
                completed.put((batch_names, result, None))
                batch = take_batch()

        def run_local(batch_names: list[str]) -> None:
            check_cancel()
            extract(pycolmap, database, images, batch_names, threads, token)
            check_cancel()
            stats["local_images"] += len(batch_names)

        def accept_remote() -> None:
            while True:
                try:
                    batch_names, result, error = completed.get_nowait()
                except Empty:
                    return
                check_cancel()
                if result is not None:
                    try:
                        import_features(database, batch_names, result, check_cancel)
                    except (NodeError, sqlite3.Error) as exc:
                        error = str(exc)
                    finally:
                        result.unlink(missing_ok=True)
                if error is None:
                    stats["remote_images"] += len(batch_names)
                    stats["remote_batches"] += 1
                    log(f"Признаки со второго ПК приняты: {len(batch_names)} кадров.")
                else:
                    stats["fallback_batches"] += 1
                    log(f"Пакет второго ПК пересчитывается на основном ({error}).")
                    run_local(batch_names)

        remote = Thread(target=run_remote, name="photo-lan-client")
        remote.start()
        try:
            batch = local_batch
            while batch is not None:
                run_local(batch[1])
                accept_remote()
                batch = take_batch()
            while remote.is_alive():
                check_cancel()
                accept_remote()
                remote.join(timeout=0.1)
            accept_remote()
        finally:
            client.stop()
            remote.join()

    if stats["local_images"] + stats["remote_images"] != len(names):
        raise NodeError("Извлечение признаков завершилось без результатов для части кадров.")
    with closing(sqlite3.connect(database)) as db:
        complete, = db.execute(
            "SELECT count(*) FROM images JOIN keypoints USING (image_id) JOIN descriptors USING (image_id)"
        ).fetchone()
    if complete != len(names):
        raise NodeError("База проекта не содержит признаки для всех кадров.")
    if stats["remote_images"]:
        stats["status"] = "distributed_with_fallback" if stats["fallback_batches"] else "distributed"
    stats["seconds"] = round(time.monotonic() - started, 3)
    return stats


class NodeServer(HTTPServer):
    allow_reuse_address = True

    def __init__(self, address, context: ssl.SSLContext, token: str, threads: int, pycolmap) -> None:
        self.context = context
        self.authorization = f"Bearer {token}"
        self.threads = threads
        self.pycolmap = pycolmap
        super().__init__(address, NodeHandler)

    def get_request(self):
        connection, address = super().get_request()
        try:
            connection.settimeout(15)
            return self.context.wrap_socket(connection, server_side=True), address
        except OSError:
            connection.close()
            raise


class NodeHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args) -> None:
        pass

    def allowed(self, path: str) -> bool:
        authorization = self.headers.get("Authorization", "").encode("latin-1")
        if not hmac.compare_digest(authorization, self.server.authorization.encode("ascii")):
            self.send_error(403)
        elif self.path != path:
            self.send_error(404)
        else:
            return True
        return False

    def begin_response(self, length: int) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        self.end_headers()

    def do_GET(self) -> None:
        if self.allowed(HEALTH_PATH):
            data = str(self.server.threads).encode("ascii")
            self.begin_response(len(data))
            self.wfile.write(data)

    def do_POST(self) -> None:
        if not self.allowed(FEATURE_PATH):
            return
        try:
            length = int(self.headers.get("Content-Length", "-1"))
            if not 0 < length <= MAX_UPLOAD_BYTES:
                raise ValueError
        except ValueError:
            self.send_error(413)
            return

        with TemporaryDirectory(prefix="spatial-photo-node-") as directory:
            root = Path(directory)
            upload, images, features = root / "images.zip", root / "images", root / "features"
            images.mkdir()
            try:
                with upload.open("xb") as stream:
                    copy_bytes(self.rfile, stream, length)
                names = unpack_upload(upload, images)
                upload.unlink()
            except (BadZipFile, NodeError, OSError):
                self.send_error(400)
                return

            print(f"Получен пакет: {len(names)} кадров.", flush=True)
            pycolmap = self.server.pycolmap
            token = pycolmap.CancellationToken()
            done = Event()

            def watch_connection() -> None:
                while not done.is_set():
                    readable, _, _ = select.select([self.connection], [], [], 0.2)
                    if readable:
                        # The client sends a single request then waits. Any
                        # further input or EOF ends work on this connection.
                        token.cancel()
                        return

            watcher = Thread(target=watch_connection, name="photo-lan-cancellation")
            watcher.start()
            try:
                database = root / "features.db"
                extract(pycolmap, database, images, names, self.server.threads, token)
                write_features(database, names, features)
            except Exception as exc:
                print(f"Ошибка пакета: {exc}", flush=True)
                self.send_error(500)
                return
            finally:
                done.set()
                watcher.join()
            self.begin_response(features.stat().st_size)
            with features.open("rb") as stream:
                copy_bytes(stream, self.wfile, features.stat().st_size)
            print(f"Вычислено и отправлено: {len(names)} кадров.", flush=True)


def node_context(root: Path) -> tuple[ssl.SSLContext, str]:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    key = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(x509.NameOID.COMMON_NAME, "Spatial Photo Node")])
    now = datetime.datetime.now(datetime.timezone.utc)
    certificate = (x509.CertificateBuilder()
                   .subject_name(subject)
                   .issuer_name(subject)
                   .public_key(key.public_key())
                   .serial_number(x509.random_serial_number())
                   .not_valid_before(now - datetime.timedelta(minutes=5))
                   .not_valid_after(now + datetime.timedelta(days=1))
                   .sign(key, hashes.SHA256()))
    key_path, cert_path = root / "key.pem", root / "certificate.pem"
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                           serialization.NoEncryption()))
    cert_path.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(cert_path, key_path)
    return context, certificate.fingerprint(hashes.SHA256()).hex()


def main() -> int:
    from hardware_profile import recommended_threads

    for stream in (sys.stdout, sys.stderr):
        if stream is not None and hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Второй ПК для Spatial Photo Cloud")
    parser.add_argument("--host", default="0.0.0.0", help="IPv4-адрес интерфейса узла")
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--threads", type=int, default=recommended_threads())
    args = parser.parse_args()
    if not 1 <= args.threads <= 64 or not 0 <= args.port <= 65535:
        parser.error("Потоки должны быть в диапазоне 1–64, порт в диапазоне 0–65535.")
    try:
        pycolmap = load_pycolmap()
        with TemporaryDirectory(prefix="spatial-photo-tls-") as directory:
            context, fingerprint = node_context(Path(directory))
        token = secrets.token_hex(16)
        with NodeServer((args.host, args.port), context, token, args.threads, pycolmap) as server:
            port = server.server_port
            addresses = {server.server_address[0]}
            if addresses == {"0.0.0.0"}:
                try:
                    addresses = {address[4][0] for address in socket.getaddrinfo(
                        socket.gethostname(), port, socket.AF_INET, socket.SOCK_STREAM,
                    ) if not address[4][0].startswith("127.")}
                except OSError:
                    addresses = set()
                if not addresses:
                    print("Укажите --host с IPv4-адресом второго ПК для подключения по сети.", flush=True)
                    addresses = {"127.0.0.1"}
            print(f"Узел готов. Потоки CPU: {args.threads}. Выберите адрес вашей сети и скопируйте код.", flush=True)
            for address in sorted(addresses):
                print(f"{address}:{port}/{fingerprint}.{token}", flush=True)
            print("Для остановки нажмите Ctrl+C или закройте окно.", flush=True)
            server.serve_forever()
    except KeyboardInterrupt:
        return 0
    except (OSError, ImportError, NodeError) as exc:
        print(f"Ошибка запуска узла: {exc}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
