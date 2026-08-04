# Artifact Store Format Specification

Status: Draft
References: ADR-009 (artifact store), ADR-010 (content addressing), ADR-025 (integrity and quarantine), ADR-015 (garbage collection)

This document defines the Spatial Platform artifact store: how artifacts are identified, laid out, written, verified, and garbage-collected. It complements `project-format.md` Section 4 and is referenced by the scheduler and worker protocol for artifact handoff.

## 1. Identity

An artifact is content-addressed. Its identity is `content_hash = SHA-256(payload bytes)`. Two writes of identical bytes produce the same hash and deduplicate to a single stored payload; distinct bytes always produce distinct artifacts. The hash is computed over exactly the payload bytes — never over the manifest, which is metadata and mutable only through replacement.

## 2. Layout

Payload and metadata are stored separately.

```
artifacts/
├── cas/<hash[0:2]>/<hash>     # payload bytes, read-only
└── <artifact_uuid>/manifest.json  # metadata
```

The payload path uses the first two hex characters of the hash as a sharding prefix. The manifest path uses the artifact UUID, not the hash, so a manifest can be rewritten (see Section 5). The `artifacts_index` table in `project.db` maps each hash to its UUID(s) and confirms both sides of the pairing.

## 3. Manifest

`manifest.json` is a single JSON object. The exact field list is:

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `artifact_uuid` | string (UUIDv4) | yes | Unique id of this artifact instance. |
| `content_hash` | string (SHA-256 hex) | yes | Identity hash of the payload. |
| `type` | string | yes | Artifact kind, e.g. `pointcloud`, `mesh`, `gaussian`, `pose_graph`. |
| `schema_version` | integer | yes | Version of the artifact schema (not of the platform). |
| `producer` | object | yes | `{ id, version, git_commit }` of the producing algorithm. |
| `input_artifact_hashes` | string[] | yes | Hashes of all inputs consumed to produce this artifact. |
| `configuration_hash` | string (SHA-256 hex) | yes | Hash of the canonical task configuration. |
| `creation_timestamp` | string (ISO-8601 UTC) | yes | When the artifact was produced. |
| `coordinate_frame` | string | yes | Frame id this artifact is expressed in. |
| `unit` | string | yes | Unit of coordinates, e.g. `meter`. |
| `file_size` | integer | yes | Payload size in bytes. |
| `mime_type` | string | yes | Payload media type, e.g. `application/x-ply`. |
| `validation_status` | string | yes | One of `valid`, `degraded`, `unverified`. |

Example:

```json
{
  "artifact_uuid": "3f4a55b6-0c2d-4e1f-8a90-1234567890ab",
  "content_hash": "ab12cd34ef56...",
  "type": "mesh",
  "schema_version": 1,
  "producer": { "id": "surface-reconstruction", "version": "1.2.0", "git_commit": "deadbeef" },
  "input_artifact_hashes": ["11aa22bb...", "33cc44dd..."],
  "configuration_hash": "55ee66ff...",
  "creation_timestamp": "2026-08-04T10:15:00Z",
  "coordinate_frame": "scene",
  "unit": "meter",
  "file_size": 1048576,
  "mime_type": "application/x-ply",
  "validation_status": "valid"
}
```

## 4. Atomic write

Artifacts are written atomically: payload to a temp file in the same filesystem, `fsync`, then rename into `cas/`. The manifest is written last, after the payload is durable. A reader therefore never observes a manifest pointing at a missing payload, and never observes a partial payload. On failure the temp file is discarded and the artifact is simply absent.

## 5. Immutability

Payloads are immutable: an existing hash is never overwritten or edited. New content means a new hash, which means a new artifact. A manifest may be replaced only when it is wrong (e.g. a mistaken producer field) and only while the artifact is unreferenced; such replacements rewrite the manifest file and are recorded in the provenance graph as a manifest amendment, never as a payload change.

## 6. Integrity

On every read the store re-verifies `SHA-256(payload) == content_hash` before handing the bytes out. A mismatch is reported as a corruption error carrying the hash and the requesting task id. The offending payload is quarantined (moved to a `quarantine/` directory) and its manifest flagged with `validation_status: degraded`, so the failure is observable and the data cannot be silently re-read.

## 7. Provenance

Provenance is stored as a directed acyclic graph whose nodes are artifacts (by hash) and whose edges are recorded in `project.db.provenance` from `input_artifact_hashes` and `configuration_hash`. Together with `producer`, the graph answers, for any artifact: "which inputs, configuration, and code produced this artifact?" The graph is append-only and versioned alongside scene snapshots in `project.db`.

## 8. GC API

Reference counting drives garbage collection. An artifact is referenced if any live manifest, any live scene version, or any job/task in `project.db` points at its hash. The GC API exposes:

- `garbage_collect(dry_run) -> plan` — lists unreferenced payloads.
- `garbage_collect(commit = true) -> stats` — deletes only unreferenced payloads and their manifests.
- `is_referenced(hash) -> bool` — single-artifact check.

Deletion requires atomic rename to `quarantine/` first, then unlink, so an interrupted GC never loses a payload that later turns out to be referenced. GC never runs while a job is executing against a project.

## 9. Answering provenance: worked example

Consider a `mesh` artifact `M` with `input_artifact_hashes: [P1, P2]`, `configuration_hash: C`, and `producer: { id: "surface-reconstruction", version: "1.2.0", git_commit: "deadbeef" }`.

1. `P1` and `P2` are themselves artifacts; their own manifests carry their producers and inputs, so the chain continues recursively to raw sensor artifacts.
2. `C` is canonicalized JSON of the surface-reconstruction configuration; identical config always yields the identical `C`.
3. Reproducing `M` means: run `surface-reconstruction@1.2.0` (git `deadbeef`) with configuration `C` over inputs `P1`, `P2`. The scheduler guarantees this by construction (cache keys in `project-format.md` Section 5 use the same triple).
4. If the same inputs and config are later run under `1.3.0`, the result hash will (almost certainly) differ; provenance identifies the code change as the cause of the new artifact, and both `M` and `M'` remain in the graph with their respective edges.
