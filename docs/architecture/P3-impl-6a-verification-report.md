# P3-impl-6a Verification Report

**Status:** PASS
**Date:** 2026-08-19
**Scope:** GTSAM 4.2.1 Isolated Dependency + Adapter Build Target

---

## 1. Deliverables

| File | Type | Lines |
|------|------|-------|
| `conanfile.txt` | Modified: added `gtsam/[>=4.2.0 <4.3.0]`, pinned `eigen/3.4.0` | +2 lines |
| `conan.lock` | Regenerated with GTSAM 4.2.1 + boost/1.84.0 |
| `CMakeLists.txt` | Modified: added `SPATIAL_HAS_GTSAM` option + `find_package(gtsam)` | +5 lines |
| `CMakePresets.json` | Modified: added `SPATIAL_HAS_GTSAM: ON` to both presets | +2 lines |
| `adapters/CMakeLists.txt` | Modified: added conditional `add_subdirectory(gtsam)` | +3 lines |
| `adapters/gtsam/CMakeLists.txt` | GTSAM adapter build target (static lib) | 45 |
| `adapters/gtsam/gtsam_optimizer_adapter.h` | Placeholder optimizer types header | 38 |
| `adapters/gtsam/gtsam_optimizer_adapter.cpp` | Placeholder adapter source | 7 |
| `adapters/gtsam/gtsam_adapter_build_info.h.in` | Build identity template | 13 |
| `tests/unit/test_gtsam_adapter.cpp` | 2 smoke tests (defaults verification) | 39 |
| `tests/CMakeLists.txt` | Modified: added conditional `spatial_gtsam_tests` target | +10 lines |

---

## 2. Test Results

| Configuration | Tests | Pass | Fail |
|---------------|-------|------|------|
| Debug (full ctest) | 537 | 537 | 0 |
| Release (full ctest) | 537 | 537 | 0 |

New tests added: **2** (GTSAM adapter smoke tests)

### New Test Breakdown

| Test | What It Proves |
|------|---------------|
| `GtsamAdapterTest.OptimizerOptionsDefaults` | `OptimizerOptions` struct default-initializes correctly (max_iterations=100, lambda_initial=1e-4, function_tolerance=1e-5) |
| `GtsamAdapterTest.OptimizerResultDefaults` | `OptimizerResult` struct default-initializes correctly (converged=false, iterations=0, final_error=0.0) |

---

## 3. Build Chain Verification

| Step | Status | Detail |
|------|--------|--------|
| Conan install Debug | PASS | GTSAM 4.2.1 built from source (pkg `a4d8c24c...`) |
| Conan install Release | PASS | GTSAM 4.2.1 shared from Debug cache |
| CMake configure Debug | PASS | `find_package(gtsam CONFIG REQUIRED)` resolves |
| CMake configure Release | PASS | Same |
| Build Debug | PASS | `spatial_gtsam_adapter.lib` + `spatial_gtsam_tests.exe` produced |
| Build Release | PASS | Same |
| ctest Debug | PASS | 537/537 |
| ctest Release | PASS | 537/537 |

---

## 4. Core Boundary Audit

| Check | Result |
|-------|--------|
| `#include.*gtsam` in `core/**` | **CLEAN** — zero matches |
| `#include.*GTSAM` in `core/**` | **CLEAN** — zero matches |
| `gtsam\|GTSAM` in `core/**/CMakeLists.txt` | **CLEAN** — zero matches |
| `gtsam` string literal in `core/trajectory/optimization.h:25` | **PASS** — field name `"gtsam"` in optimizer backend enum, not a dependency |

**Verdict:** `spatial_core` is completely GTSAM-free. The only GTSAM reference in `core/` is a string literal in the optimizer backend enum, which is a value type — not a build or header dependency.

---

## 5. Dependency Isolation

| Target | Links GTSAM? | Reason |
|--------|-------------|--------|
| `spatial_core` | NO | Core boundary invariant maintained |
| `spatial_engine` | NO | Engine depends on core, not adapters |
| `spatial_gtsam_adapter` | YES (PRIVATE) | Only target that links `gtsam::gtsam` |
| `spatial_gtsam_tests` | Transitively via adapter | Adapter linked PRIVATE; boost_test_exec_monitor still present but isolated |
| `spatial_unit_tests` | NO | Unchanged from P3-impl-3 baseline |
| `spatial_adapter_tests` | NO | Tests COLMAP adapter only |
| All other targets | NO | Unchanged |

---

## 6. Build Workarounds Documented

### 6.1 GTSAM CMake target name
Conan's GTSAM package declares `gtsam::gtsam` (lowercase), not `GTSAM::GTSAM`. The `find_package` call uses lowercase `gtsam`.

### 6.2 boost_test_exec_monitor collision
GTSAM's transitive Boost dependency links `boost_test_exec_monitor`, which:
- Defines its own `main()` that intercepts `--gtest_list_tests`, breaking `gtest_discover_tests`
- Expects a `int test_main(int, char** const)` symbol

**Workaround:** The GTSAM test target uses `add_test()` instead of `gtest_discover_tests()`, provides its own `main()` via `::testing::InitGoogleTest()`, and stubs `test_main()`.

### 6.3 GTest/Boost.Test macro conflict
Boost.Test headers redefine the `TEST` macro. The test file includes `<gtest/gtest.h>` **before** the adapter header to ensure GTest's macro wins.

### 6.4 CMakeUserPresets.json conflict
Conan generates `CMakeUserPresets.json` with two includes that both define `conan-default`, causing a duplicate preset error. Removed since our `CMakePresets.json` already references the toolchain files directly.

### 6.5 Conan profile cppstd
The Conan profile was updated from `cppstd=14` to `cppstd=17` because GTSAM 4.2.1 and gtest 1.18.0 require C++17 minimum. C++20 was rejected due to MSVC C3878 errors in GTSAM's `chartTesting.h`.

---

## 7. Artifacts Produced

```
adapters/gtsam/Debug/spatial_gtsam_adapter.lib    (Debug static lib)
adapters/gtsam/Release/spatial_gtsam_adapter.lib  (Release static lib)
tests/Debug/spatial_gtsam_tests.exe               (Debug test binary)
tests/Release/spatial_gtsam_tests.exe             (Release test binary)
```

---

## 8. Readiness for P3-impl-6b

The GTSAM adapter build target is proven end-to-end. The following are ready for P3-impl-6b (Levenberg-Marquardt optimizer adapter):

- [x] GTSAM 4.2.1 available via Conan in both Debug and Release
- [x] `spatial_gtsam_adapter` static lib compiles and links
- [x] `spatial_gtsam_tests` test target runs and passes
- [x] `spatial_core` boundary intact — zero GTSAM leakage
- [x] Build presets configured with `SPATIAL_HAS_GTSAM=ON`
- [ ] `gtsam_optimizer_adapter.h` is a placeholder — needs real types in 6b
- [ ] No optimization logic implemented yet — deferred to 6b
