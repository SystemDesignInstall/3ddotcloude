# Spatial Capture transport v1 (experimental)

This standalone transport is confined to `mobile/android`. It is not a new Core schema, canonical artifact or public Platform API. Adoption by Scene/CAS requires a dedicated importer/RFC where protected contracts change.

A ZIP contains `session.json`, `frames/000000/...`, and, on successful stop, `cloud.ply` and `manifest.json`. All paths are relative. No remote endpoints or absolute host paths are persisted.

Each committed frame contains:

- `frame.json`: string nanosecond camera/depth timestamps, image sizes, intrinsics, pose, raw-depth-to-image mapping and SHA-256 hashes of its four payloads.
- `depth.u16le`: tightly packed width × height unsigned 16-bit **millimeters along the optical axis**, zero means missing.
- `confidence.u8`: width × height unsigned confidence; not an uncertainty in meters.
- `color.nv21.gz`: lossless gzip of packed camera Y followed by interleaved VU, dimensions in metadata.
- `color.jpg`: lossy JPEG 95 convenience copy; raw NV21 is authoritative for reproducing color.

Camera coordinates use OpenCV (+X right, +Y down, +Z forward). World is the right-handed ARCore local session frame, +Y up, no geographic alignment. Matrices are column-major and named `world_from_camera_cv`; p_world = T * p_camera_cv. Conversion at the capture boundary is `world_from_camera_gl * diag(1,-1,-1,1)`. Camera intrinsics refer to unrotated source images. Depth intrinsics are scaled texture intrinsics; `texture_to_image_corners` maps normalized texture corners (0,0), (1,0), (0,1), (1,1) into CPU image pixels and preserves crop/aspect differences.

Timestamps belong to ARCore's device monotonic domain, not Unix time. Camera and depth timestamps are retained separately. Only unique depth timestamps are recorded. New session UUIDs define separate local frames; no automatic relation between sessions is asserted. Metric basis is `ARCore estimated metric scale; not externally validated`.

The preview cloud retains the highest-confidence observation per 0.025 m voxel, uses confidence >=128, 0.3–5 m depth, and samples up to roughly 12000 candidate depth pixels per accepted frame. It is bounded at 300000 points. PLY contains float32 XYZ in meters plus uchar RGB and confidence (16 bytes/vertex). No global correction or loop-closure adjustment is applied. Frames/poses are retained to permit future reprocessing. Recovery in Python uses double arithmetic, so boundary voxel assignments can differ from Java float32; it is not advertised as a bit-identical canonical Platform reconstruction.

## Crash and publication semantics

Frame files are first written to `.pending-NNNNNN`, their metadata is written last, then the directory is renamed under `frames/`. Incomplete pending directories are excluded from export. No committed observation is overwritten. The app writes the final cloud to a `.partial` file and renames it, then commits `manifest.json`. Metadata/raw depth writes call fsync; this is process-crash resilience, not a guarantee against physical device/storage failure. A missing final manifest means recovery is required, not success. The desktop importer validates payload hashes and rigid transforms, limits sizes/decompression, rejects unsafe ZIP paths and publishes its output by directory rename only after validation.
