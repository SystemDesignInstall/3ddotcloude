# Building the Spatial Platform

- **Status:** ratified (P0)
- **References:** ADR-003 (Conan 2 dependency manager), ADR-022 (toolchain bootstrapping), ADR-017 (cross-platform), ADR-031 (M0 scope)
- **M0 dependencies (exact):** `eigen`, `protobuf`, `sqlite3`, `nlohmann-json`, `gtest`. Anything else fails the dependency-registry gate.

## 1. Reproducibility contract

The same git commit on the same platform must produce a byte-identical toolchain on a developer machine and in CI. That means: **pinned versions only** (`conan.lock` committed), **profiles committed per platform**, and **CMake presets as the only supported way to configure**. Ad hoc configuration is a review failure.

## 2. Windows prerequisites

| Tool | Version | Notes |
|---|---|---|
| Git | current | Git for Windows |
| Visual Studio 2022 Build Tools | MSVC 17.x | **C++ workload** (MSVC compiler, Windows SDK, CMake tools optional) |
| CMake | >= 3.28 | presets required |
| Python | 3.11 | for the SDK and tooling |
| Conan 2 | via `pip` | `python -m pip install conan` |

## 3. Linux prerequisites

| Tool | Version | Notes |
|---|---|---|
| gcc | 12 | clang 16 is the alternative toolchain |
| clang | 16 | CI images define both; gcc-12 default |
| CMake | >= 3.28 | presets required |
| Python | 3.11 | for the SDK and tooling |
| Conan 2 | via `pip` | `python -m pip install conan` |

## 4. Conan profile

Conan 2 profiles are committed per platform; do not hand-edit them. Install and verify:

```
conan profile list                # should show msvc2022-x64 (Windows) / linux-gcc12 (Linux)
conan profile show msvc2022-x64   # host + build settings, toolchain reference
```

If the profiles are not present, re-import them from the committed profile files under the repository tooling directory (never re-create ad hoc).

## 5. Build (both platforms)

From the repository root:

```
conan install . --build=missing
cmake --preset <preset>
cmake --build --preset <preset>
```

- `conan install . --build=missing` resolves and locks all dependencies from `conan.lock`, validates them against `THIRD_PARTY.yml` (license + status), and generates the toolchain file the presets reference.
- The **only** supported presets are `default` and `release` (see `CMakePresets.json`):
  - `default` — Debug build, tests enabled, warnings-as-errors, clang-tidy/clang-format available.
  - `release` — Release build, tests enabled, assertions off.
- Configure through a preset only. Direct `cmake -S .` without a preset is rejected by policy (ADR-022).
- Python SDK tooling (`mypy`, `ruff`, `pytest`) runs from its own pinned lockfile environment, not the system Python.

## 6. Platform notes

- **Windows:** run the commands from the same shell where the VS Build Tools environment is available (or rely on the preset selecting the MSVC toolchain). Native file locking and WAL behavior of SQLite are exercised by the test suite on both platforms.
- **Linux:** gcc-12 is the default; clang-16 is verified in the CI matrix with ASan/UBSan enabled in Debug (ADR-016).

## 7. Dependency registry

- `THIRD_PARTY.yml` is the single registry of record for dependencies and backends (ADR-003). CI's `dep-registry-validation` gate rejects any resolved dependency that is not registered.
- The M0 **kernel** may link only `eigen`, `protobuf`, `sqlite3`, `nlohmann-json`, `gtest`. Backends (COLMAP, OpenMVS, GTSAM, ...) are registered as status `planned` and are never built in M0.

## 8. Verify the build

```
ctest --test-dir build/<preset> --output-on-failure   # kernel + integration
pytest                                                # Python SDK
```

See `docs/development/testing.md` for the full test matrix and CI gates.

## References

- `docs/development/testing.md`, `docs/architecture/storage-model.md`
- ADR-003, ADR-016, ADR-017, ADR-022
