# A2 mapper — integration and evaluation

Date: 2026-11-08. Decision: external reference and documented export import.
This is an implementation/roadmap record, not a ratified Core contract change.

Upstream: Werdna0107/A2-slam, v4, commit
`5212867f9fb7198d55f0a08f72aa0f381c11fa67`.
Public content contains binaries and documentation, not source or benchmark data.
Do not claim an audited odometry algorithm or a general loop-closure implementation.
The published stationary start/end tie is distinct from place recognition, multiple
loops and multi-session registration. Android processes external scanner recordings;
it does not reconstruct space from the phone camera.

## Adopted work and explicit status

| Work | Current delivery | Remaining acceptance |
|---|---|---|
| Whole export folder | GUI + CLI import LAS/TXT with optional selected CSV | Actual SHARE/A2 recordings; generic raw-capture discovery is separate |
| Cloud + trajectory in one session | Portable bundle with verified reopening and read-only joint viewer; full-route sampled overlay | Core session/scene/CAS publication |
| Control points with time | SHARE/A2 parsing; explicit rejection report; RTK-fixed filtering | Datum/time/lever-arm calibration and correction |
| Original and corrected versions | Original source snapshots retained; no correction fabricated | Immutable derived corrected trajectory and rebuilt cloud |
| Color workflow | Existing LAS RGB preserved in display PLY; 8/16-bit choice | Independent image-based colorization with calibration/visibility |
| Simple operator flow | One import dialog, cancellation, local route/quality report | Real Windows/device acceptance |
| External A2 evaluation | Recorded in registry, benchmark protocol below | Results on actual common datasets |

## Canonical integration requirements

The current C++ importer accepts camera-center TUM poses matching existing camera
Frames. A2 exports have an unverified scanner-reference pose. Renaming/reordering
columns is insufficient and would violate the frame/time contracts.

Before Core integration, establish:

1. The pose reference (LiDAR, IMU or rig), transform direction, axes, quaternion
   convention and RPY order using known rotations/translations and author documentation.
2. Time origin/domain and a calibrated relation to camera/LiDAR/GNSS clocks.
   Keep original int64 nanoseconds and version the time transformation.
3. CRS, units, axis order, vertical datum and ENU origin. Do not infer the ENU origin
   from the first accepted point after filtering: it may differ from the producer's origin.
4. Source sensor/rig identities and calibrated extrinsics to any camera pose consumer.
5. A canonical non-camera trajectory ingest contract and review for protected schema
   changes. Retain source artifacts, configuration hash, validation status and lineage.
6. LAS point attributes and per-point time availability. A trajectory correction cannot
   be faithfully transferred to an accumulated cloud without point-to-pose association
   or access to the original scans and motion compensation information.

The research bundle is deliberately outside `.spx`, CAS and canonical schemas.
Do not advertise its UUID/JSONL as an existing Core artifact or scheduler capability.
No A2 binary is embedded, automated through guessed CLI arguments or reverse engineered.

## Independent control-point correction design

- Separate fitting controls from independent checkpoints before estimation.
- Preserve coordinate and time calibration independently of observations.
- First estimate rigid registration only when scale is verified; arbitrary scale must
  not be silently fitted to metric LiDAR data. Establish rank/coverage checks.
- Use uncertainty-weighted robust residuals; explicitly report exclusions and weights.
- Treat lever arm and time offset as calibrated parameters or independently observable
  unknowns; do not promise automatic estimation from any three points.
- A trajectory optimizer may combine local odometry constraints, validated loop edges
  and control-point constraints. Existing GTSAM abstractions are candidate building
  blocks, but LiDAR support is not established by the existing camera tests.
- Generate a new trajectory artifact. Rebuild a new cloud from original measurements
  after correction; preserve both versions and their input/procedure hashes.
- Never force endpoint closure on an open route; never report endpoint coincidence
  as independent accuracy or as evidence of multiple-session registration.

## Independent colorization design

Register each camera intrinsics/extrinsics calibration and clock relation; associate
images to scan poses; project points using explicit transforms; reject occluded,
behind-camera, out-of-image and low-confidence samples; record selected observations
and quality masks. Preserve the original uncolored geometry and generate a new colored
artifact. Importing RGB already present in LAS does not implement this pipeline.

## Benchmark protocol

Keep acquisition sources unchanged, hash inputs and engine versions, use identical
datasets and record CPU/GPU/RAM, settings, wall time, peak RAM and failures. Do not
infer AMD acceleration from the availability of a Windows/Android executable.

| Dataset | Principal measurement |
|---|---|
| Room with independent surveyed dimensions | Distance residuals, plane thickness, doubled surfaces |
| Long repetitive corridor | Drift and local consistency at surveyed checkpoints |
| Closed loop with stationary ends | Intermediate geometry as well as endpoint gap |
| Open route | No fabricated closure; explicit tracking/correction failure |
| Survey with distributed controls/checkpoints | Independent checkpoint RMSE/max error, fitted-point residuals separately |
| Colored capture with sharp boundaries | Geometric/color edge displacement and occlusion errors |
| Separate sessions with overlap | Registration failure/success and independent alignment errors |

For each report state which points were used to fit, which were held out, the coordinate
and height systems, time alignment, unit/scale evidence and unknown quantities.
No universal centimetre threshold is invented here: set task tolerances before testing.
Missing ground truth produces `not_assessed`, never a zero-error score.

## Verification record

Synthetic LAS 1.2 formats 0/3 and LAS 1.4 format 7, large coordinates with millimetre
differences, 8/16-bit RGB, decimal epoch timestamps, malformed inputs, CSV rejections,
RTK status, source mutation, cancellation, CLI and viewer compatibility are covered by
`python/research/photo_to_cloud/test_a2_import.py`.
The reopening slice adds `test_capture_session.py`: portable directories, changed cloud
and trajectory rejection, missing/legacy manifests, full-route sampling with exact time,
HTTP read-only protection, and actual viewer scripts/parser under a Node DOM/WebGL shim.
The shim checks overlay alignment with a real parsed LAS-derived PLY before/after
rotation, opt-in controls, export blocking and switching back to a standalone PLY.
Full local research suite: 113 tests passed, including 29 A2 importer tests and 9 capture
session tests. Architecture constitution/dependency/domain/worker/debt and schema checks
are run separately; see PR checks for the current revision.
Native GUI smoke execution was attempted but this executor has no DISPLAY/X server;
system Xvfb installation is unavailable due process permission restrictions.
The preceding importer revision passed Windows packaging and packaged GUI smoke tests
in Actions run 42488156429. The reopening revision requires its own CI result.
Full browser rendering was not locally verified: Chromium download returned an invalid
archive. Actual A2 export accuracy and interactive Windows acceptance remain unverified.
