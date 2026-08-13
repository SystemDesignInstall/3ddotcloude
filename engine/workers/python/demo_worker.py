#!/usr/bin/env python3
"""M0 demo worker (RFC-0003 S5.7, worker-protocol S8).

Implements the framed protobuf worker protocol end-to-end: WorkerHello on
startup, one TaskRequest at a time (max_concurrency = 1), progress 0 -> 100,
a deterministic payload artifact handed back as a content hash, 1-second
heartbeats, cooperative cancellation, and clean shutdown.

The wire format is encoded directly (spatial_wire) so the worker runs without
the google.protobuf Python runtime; the generated worker_pb2 module is still
the canonical contract (schemas/protobuf/worker.proto).
"""

import hashlib
import json
import os
import queue
import struct
import sys
import threading
import time
import uuid

import spatial_wire

PROTOCOL_VERSION = 1
MAX_FRAME = 1 << 26  # 64 MiB protocol guard (matches the C++ side)
OUTPUT_NAME = "output.txt"

write_lock = threading.Lock()
inbound = queue.Queue()


def read_frame(stream):
    prefix = stream.read(4)
    if not prefix:
        return None  # EOF
    if len(prefix) != 4:
        raise RuntimeError("truncated frame prefix")
    (length,) = struct.unpack("<I", prefix)
    if length > MAX_FRAME:
        raise RuntimeError("frame exceeds protocol limit")
    body = stream.read(length)
    if len(body) != length:
        raise RuntimeError("truncated frame body")
    return body


def write_message(payload):
    with write_lock:
        sys.stdout.buffer.write(struct.pack("<I", len(payload)) + payload)
        sys.stdout.buffer.flush()


def hello():
    write_message(
        spatial_wire.worker_hello(
            PROTOCOL_VERSION, str(uuid.uuid4()), ["demo_task"], 2, 1024, 0,
            sys.platform, sys.maxsize > 2**32 and "64-bit" or "32-bit", 1
        )
    )


def run_task(req, cancel_event):
    task_id = req["task_id"]
    workspace = req["workspace"]
    write_message(spatial_wire.task_accepted(task_id))

    # C1-S1: consume declared inputs from the materialized workspace
    # (workspace/inputs/<hash>). The worker only ever sees its workspace and
    # never the CAS or a database handle (worker-boundary).
    input_hashes = req.get("input_refs", [])
    for ref in input_hashes:
        path = os.path.join(workspace, "inputs", ref)
        if not os.path.isfile(path):
            write_message(
                spatial_wire.task_failed(
                    task_id, "INPUT_MATERIALIZED_FILE_MISSING",
                    "declared input missing from workspace: %s" % ref, False
                )
            )
            return
    input_payloads = []
    for ref in input_hashes:
        with open(os.path.join(workspace, "inputs", ref), "r",
                  encoding="utf-8") as fh:
            input_payloads.append(fh.read())

    for percent in range(0, 101, 10):
        if cancel_event.is_set():
            write_message(
                spatial_wire.task_progress(task_id, percent, substage="cancelled")
            )
            return
        write_message(
            spatial_wire.task_progress(task_id, percent, substage="execute")
        )
        time.sleep(0.05)

    payload = "demo:" + task_id + ":" + req["task_type"] + ":" + req["spec_json"]
    for p in input_payloads:
        payload += "|input:" + p
    content_hash = hashlib.sha256(payload.encode("utf-8")).hexdigest()

    os.makedirs(workspace, exist_ok=True)
    path = os.path.join(workspace, OUTPUT_NAME)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(payload)

    artifact_id = str(uuid.uuid4())
    size = len(payload.encode("utf-8"))
    manifest = json.dumps({
        "artifact_uuid": artifact_id,
        "content_hash": content_hash,
        "type": "demo_output",
        "schema_version": 1,
        "producer": {"id": "demo-worker", "version": "0.1.0",
                     "git_commit": "demo"},
        "input_artifact_hashes": input_hashes,
        "configuration_hash": None,
        "creation_timestamp": "1970-01-01T00:00:00Z",
        "coordinate_frame": "",
        "unit": "meter",
        "file_size": size,
        "mime_type": "text/plain",
        "validation_status": "valid",
    })
    write_message(
        spatial_wire.task_artifact(task_id, artifact_id, content_hash,
                                   "demo_output", size, "text/plain",
                                   manifest, path)
    )
    write_message(
        spatial_wire.task_completed(
            task_id,
            [spatial_wire._artifact_info(artifact_id, content_hash,
                                         "demo_output", size, "text/plain",
                                         manifest, path)],
        )
    )


def reader():
    stream = sys.stdin.buffer
    while True:
        try:
            frame = read_frame(stream)
        except Exception:
            return
        if frame is None:
            return
        inbound.put(frame)


def heartbeater(stop):
    while not stop.is_set():
        time.sleep(1.0)
        try:
            write_message(spatial_wire.heartbeat("", time.time_ns(), 0))
        except Exception:
            return


def main():
    hello()
    stop = threading.Event()
    threading.Thread(target=reader, daemon=True).start()
    threading.Thread(target=heartbeater, args=(stop,), daemon=True).start()

    active_task = None
    task_cancel = threading.Event()
    task_thread = None
    while True:
        try:
            frame = inbound.get(timeout=0.2)
        except queue.Empty:
            if task_thread is not None and not task_thread.is_alive():
                task_thread = None
                active_task = None
                task_cancel = threading.Event()
            continue

        name, payload = spatial_wire.parse_worker_message(frame)
        if name == "task_request":
            if active_task is None:
                active_task = spatial_wire.parse_task_request(payload)
                task_cancel = threading.Event()
                task_thread = threading.Thread(
                    target=run_task, args=(active_task, task_cancel), daemon=True
                )
                task_thread.start()
        elif name == "task_cancelled":
            cancelled = spatial_wire.parse_task_cancelled(payload)
            if active_task is not None and cancelled["task_id"] == active_task["task_id"]:
                task_cancel.set()
        elif name == "shutdown":
            break

    stop.set()
    if task_thread is not None:
        task_thread.join(timeout=1.0)
    # os._exit, not sys.exit: a daemon thread blocked on stdin.buffer.read()
    # triggers `_enter_buffered_busy` (access violation) during interpreter
    # finalization. stdout is flushed per frame, so nothing is lost.
    os._exit(0)


if __name__ == "__main__":
    main()
