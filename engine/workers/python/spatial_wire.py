# Minimal protobuf wire-format codec (proto3) used by the demo worker and the
# worker integration test fixtures. It exists because the worker must not
# depend on the google.protobuf Python runtime (not guaranteed on the host);
# field numbers below are part of the frozen worker protocol and mirror
# schemas/protobuf/worker.proto exactly (ADR-011/012).

# Wire types used by this schema: 0 = varint, 2 = length-delimited.
_WIRE_VARINT = 0
_WIRE_LENGTH = 2


def vint(value):
    """Encodes a non-negative integer as a base-128 varint."""
    value &= 0xFFFFFFFFFFFFFFFF
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def _key(field, wire_type):
    return vint((field << 3) | wire_type)


def f_varint(field, value):
    return _key(field, _WIRE_VARINT) + vint(value)


def f_string(field, value):
    data = value.encode("utf-8")
    return _key(field, _WIRE_LENGTH) + vint(len(data)) + data


def f_message(field, payload):
    return _key(field, _WIRE_LENGTH) + vint(len(payload)) + payload


def read_varint(data, pos):
    result = 0
    shift = 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7
        if shift >= 70:
            raise ValueError("varint too long")


def parse_fields(data):
    """Parses a message into {field_number: [(kind, value), ...]}."""
    fields = {}
    pos = 0
    while pos < len(data):
        tag, pos = read_varint(data, pos)
        field = tag >> 3
        wire = tag & 7
        if wire == _WIRE_VARINT:
            value, pos = read_varint(data, pos)
            fields.setdefault(field, []).append(("varint", value))
        elif wire == _WIRE_LENGTH:
            length, pos = read_varint(data, pos)
            value = data[pos:pos + length]
            pos += length
            fields.setdefault(field, []).append(("bytes", value))
        else:
            raise ValueError("unsupported wire type %d" % wire)
    return fields


def _string_field(fields, field, default=""):
    for kind, value in fields.get(field, []):
        if kind == "bytes":
            return value.decode("utf-8")
    return default


def _bytes_field(fields, field):
    for kind, value in fields.get(field, []):
        if kind == "bytes":
            return value
    return b""


# WorkerMessage oneof payloads (worker.proto):
#   hello=1 task_request=2 task_accepted=3 task_progress=4 task_log=5
#   task_artifact=6 task_completed=7 task_failed=8 task_cancelled=9
#   heartbeat=10 shutdown=11


def worker_hello(protocol_version, worker_id, capabilities, cpu_cores, ram_mb,
                 gpu_mem_mb, os_name, arch, max_concurrency):
    resources = (
        f_varint(1, cpu_cores) + f_varint(2, ram_mb) + f_varint(3, gpu_mem_mb)
        + f_string(4, os_name) + f_string(5, arch)
    )
    caps = (
        b"".join(f_string(1, c) for c in capabilities) + f_message(2, resources)
        + f_varint(3, max_concurrency)
    )
    payload = f_varint(1, protocol_version) + f_string(2, worker_id) + f_message(3, caps)
    return f_message(1, payload)


def task_accepted(task_id):
    return f_message(3, f_string(1, task_id))


def task_progress(task_id, percent, substage="", message=""):
    payload = f_string(1, task_id) + f_varint(2, percent)
    if substage:
        payload += f_string(3, substage)
    if message:
        payload += f_string(4, message)
    return f_message(4, payload)


def _artifact_info(artifact_id, content_hash, artifact_type, size, mime,
                   manifest_json):
    payload = (
        f_string(1, artifact_id) + f_string(2, content_hash)
        + f_string(3, artifact_type) + f_varint(4, size) + f_string(5, mime)
        + f_string(6, manifest_json)
    )
    return payload


def task_artifact(task_id, artifact_id, content_hash, artifact_type, size,
                  mime, manifest_json):
    info = _artifact_info(artifact_id, content_hash, artifact_type, size, mime,
                          manifest_json)
    return f_message(6, f_string(1, task_id) + f_message(2, info))


def task_completed(task_id, artifact_infos):
    payload = f_string(1, task_id) + b"".join(f_message(2, i) for i in artifact_infos)
    return f_message(7, payload)


def task_failed(task_id, code, message_text, recoverable):
    err = (f_string(1, code) + f_string(2, message_text)
           + f_varint(4, 1 if recoverable else 0))
    return f_message(8, f_string(1, task_id) + f_message(2, err))


def task_cancelled(task_id, reason=""):
    payload = f_string(1, task_id)
    if reason:
        payload += f_string(2, reason)
    return f_message(9, payload)


def heartbeat(worker_id, timestamp_ns, rss_bytes):
    payload = (f_string(1, worker_id) + f_varint(2, timestamp_ns)
               + f_varint(3, rss_bytes))
    return f_message(10, payload)


def parse_task_request(data):
    fields = parse_fields(data)
    return {
        "task_id": _string_field(fields, 1),
        "task_type": _string_field(fields, 2),
        "spec_json": _string_field(fields, 3),
        "workspace": _string_field(fields, 4),
    }


def parse_task_cancelled(data):
    fields = parse_fields(data)
    return {
        "task_id": _string_field(fields, 1),
        "reason": _string_field(fields, 2),
    }


def parse_worker_message(data):
    """Returns (payload_name, payload_bytes) for inbound messages, else
    (None, None). Only the engine->worker payloads are decoded here."""
    fields = parse_fields(data)
    for field, name in ((2, "task_request"), (9, "task_cancelled"), (11, "shutdown")):
        for kind, value in fields.get(field, []):
            if kind == "bytes":
                return name, value
    return None, None
