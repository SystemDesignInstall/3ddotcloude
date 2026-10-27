# Building Spatial Platform

Updated: 2026-10-27. References: ADR-003, ADR-017, ADR-022, RFC-0003.

The supported build uses the committed Conan profiles, `conan.lock` and CMake
presets. All four presets enable tests, warnings as errors and the real GTSAM
adapter. Core remains independent of GTSAM and COLMAP.

## Prerequisites

- Git and a C++20 compiler: Visual Studio 2022 Build Tools on Windows, GCC 13
  on Linux (Ubuntu 24.04).
- Python 3.12, including `pip`. Python also runs the worker protocol tests.
- Build tools matching CI:

```sh
python -m pip install conan==2.33.0 cmake==4.4.4 ninja==1.13.2
```

Ensure the scripts installed by pip are available on `PATH`.
Run the commands from the repository root. A named default Conan profile is
not required: every install below supplies the committed host and build
profiles explicitly. `--build=missing` may compile dependencies on a first
checkout; allow time and disk space for Boost, Protobuf and GTSAM.

## Windows

Install the Visual Studio C++ workload and Windows SDK. The presets select
the Visual Studio 17 2022 generator and x64 target. Debug:

```sh
conan install . --lockfile=conan.lock --profile:host=conan/profiles/msvc2022-x64-debug --profile:build=conan/profiles/msvc2022-x64 --output-folder=build/conan-debug --build=missing --conf:all=tools.build:jobs=2
cmake --preset default
cmake --build --preset default --parallel 2
ctest --preset default --parallel 2 --timeout 180
```

Release:

```sh
conan install . --lockfile=conan.lock --profile:host=conan/profiles/msvc2022-x64 --profile:build=conan/profiles/msvc2022-x64 --output-folder=build/conan --build=missing --conf:all=tools.build:jobs=2
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release --parallel 2 --timeout 180
```

The test presets supply the configuration explicitly (`Debug` / `Release`)
for the multi-configuration generator. The CLI copies its required Windows
runtime DLLs alongside the executable. GTSAM test targets receive their runtime
search paths from Conan.

## Linux

Use GCC 13 and Ninja. Debug:

```sh
conan install . --lockfile=conan.lock --profile:host=conan/profiles/linux-gcc13-debug --profile:build=conan/profiles/linux-gcc13 --output-folder=build/conan-linux-debug --build=missing --conf:all=tools.build:jobs=2
cmake --preset linux-debug
cmake --build --preset linux-debug --parallel 2
ctest --preset linux-debug --parallel 2 --timeout 180
```

Release:

```sh
conan install . --lockfile=conan.lock --profile:host=conan/profiles/linux-gcc13 --profile:build=conan/profiles/linux-gcc13 --output-folder=build/conan-linux-release --build=missing --conf:all=tools.build:jobs=2
cmake --preset linux-release
cmake --build --preset linux-release --parallel 2
ctest --preset linux-release --parallel 2 --timeout 180
```

The platform and adapters compile as C++20. Linux builds GTSAM itself as
C++17: its bundled Spectra in 4.2.1 contains constructors rejected by GCC in
C++20 mode. This is a dependency setting, not a vendor-source patch or a change
to the platform language standard.

Linux profiles select stable, serial GTSAM: no unstable module, CppUnitLite,
TBB/hwloc or METIS. Boost.Stacktrace is also disabled. These optional facilities
are not used by the platform's trajectory / bundle-adjustment adapters. GTSAM
still performs real optimization; it is not replaced with a stub. Windows
profiles retain their existing dependency options. This is not a claim of
bit-identical numerical results between platforms or dependency options.

MSVC-specific warning flags and DLL copy commands are guarded by platform.
Generated Protobuf source and GTSAM adapter compilation allow warnings from
third-party code; other platform code retains warnings as errors.

## Dependencies and verification

Direct dependencies are Eigen, SQLite, nlohmann_json, GTest, Protobuf and GTSAM.
The lockfile pins recipe versions and revisions, including transitive tools.
Do not use `--lockfile-partial` for a release or routine verification. A new
platform dependency requires an explicit reviewed lockfile update.

The executable tests include real child processes, CAS corruption, pipeline
failure, trajectory optimization and the sparse-correction CLI. Native COLMAP
is optional; the process probe is not proof of native reconstruction quality.

Architecture gates:

```sh
python scripts/check_dependencies.py --repo .
python scripts/check_schemas.py --repo .
python scripts/check_worker_boundary.py --repo .
python scripts/check_arch_debt.py --repo .
python scripts/check_domain_types.py --repo .
python scripts/check_rfc.py --rfcs spatial-rfcs
python scripts/check_constitution.py --repo . --rfcs spatial-rfcs --base main --rfc RFC-0003
```

Use the RFC governing the actual change for constitution checks. CI checks out
the current `SystemDesignInstall/spatial-rfcs` governance repository separately
and obtains the RFC reference from the change description or commit messages.
It runs real configure/build/CTest jobs on Ubuntu and Windows in both
configurations. Python SDK checks are not advertised: that SDK is not present
in the current repository.

See [current verification report](../architecture/P3.1-reliability-verification-2026-10-27.md)
for executed results and their limits.
