# Android capture preview verification

Version: 0.1.0-preview.1. Base: afa34beb7dc9c46b84cea60967e36be17efa6e63.

- Clean Android debug build: PASS (JDK 17, AGP 8.7.3, Gradle 8.9, API/build-tools 35).
- `:core:check`: PASS; 17 executable golden assertions covering camera/world axes, transform direction, unsigned depth, NV21 color, confidence/voxel limits and PLY byte layout.
- Desktop importer: 4 tests PASS (recovery geometry/color, checksum failure with atomic output, ZIP path traversal rejection, preservation of existing destination).
- Existing SpatialPhotoCloud `preview.sample_ply`: PASS on Java-generated PLY including confidence property.
- Android Lint: 0 errors, 18 warnings. Remaining warnings concern portrait-only phone layout, RU-only programmatic text and conservative free-space checks. No lint baseline or disabled error checks.
- APK signature: v2 verification PASS; minimum Android API 24, target API 35.
- ZIP alignment: `zipalign -c -P 16 4` PASS.
- Dependency registry, worker boundary, architecture debt and diff whitespace: PASS.

APK: `Spatial_Capture_0.1.0_preview1.apk`, 899325 bytes.
SHA-256: `eeb92855f2d585d23714acf8a3571c9248a6cafc36712affdfbe1d120ff4c3fc`.

Not tested: physical Android camera/ARCore session, sensor-specific crop/stride, real scan accuracy, relocalization, prolonged heat/memory, filesystem provider export, and OEM process-kill behavior. No emulator result is presented as a real scan. This is a signed development APK for device validation, not a stable commercial release.

No RTAB-Map/Open3D/COLMAP integration, mesh generation, cross-session alignment or native Scene/CAS import is claimed. Desktop import is staging/validation only. Original observations allow these later steps without recapture.
