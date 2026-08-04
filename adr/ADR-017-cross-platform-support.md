# ADR-017 — Cross-Platform Support

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Spatial Platform targets a commercial market where the operator workstation is Windows (MSVC) and the server/render-farm and CI are Linux. Third-party backends (COLMAP, OpenMVS, GTSAM, Ceres, Open3D) build on both platforms but with different toolchains and packaging. The worker protocol must behave identically on both platforms, which rules out Unix-only mechanisms. Paths and Uris must be portable. Sanitizers are available on Linux but not practically on Windows MSVC. The platform must avoid `#ifdef` sprawl in business logic and must keep the kernel free of platform branches.

## Decision

Windows (MSVC 2022, VS 2022 Build Tools) and Linux (gcc-12 and clang-16) are both first-class, from M0 onward. CI is a GitHub Actions matrix covering Ubuntu+Windows x Debug+Release, with sanitizers (ASan/UBSan) enabled on Linux jobs only. All file references in business logic use the portable `Uri` abstraction; the filesystem layer centralizes path handling in `core` so no component does its own path string manipulation. The worker protocol uses length-prefixed Protobuf frames `[u32 LE len][proto]` over stdin/stdout in both OSes — deliberately avoiding Unix sockets vs named pipes divergence, so workers and test harnesses are identical across platforms. Process spawning, cancellation, and crash detection are implemented once behind a small platform layer (Win32 and POSIX) and never leak into algorithms or the scheduler. Toolchains are pinned: MSVC 17.x on Windows, gcc-12/clang-16 on Linux, CMake >= 3.28, Conan 2 profiles per platform (ADR-022). macOS is not a target; introducing it later requires a ratified RFC that re-evaluates paths, worker spawning, and the backend registry.

## Alternatives

- **Linux-only, Windows later:** rejected — the desktop market is Windows-first and porting later is more expensive than carrying both from M0.
- **A cross-platform framework layer (e.g. Qt/CORBA-style IPC):** rejected — heavy, and the protobuf-over-stdio worker protocol already solves IPC portably.
- **Conditional compilation in kernel code:** rejected — platform concerns belong in the platform layer; `core/**` and `engine/**` stay platform-neutral, keeping Architecture Debt at zero.

## Consequences

- Positive: identical worker protocol on both platforms reduces the CI matrix surface; portable Uris keep storage and scheduler code shared; sanitizers give the Linux jobs stronger memory checking; kernel stays platform-neutral.
- Negative: two toolchains and two packaging pipelines to maintain; some dependencies have Windows-only quirks that must be tracked in the registry; sanitizer coverage is weaker on Windows.
- Risks and mitigations: risk of path/uri divergence — mitigated by a portability lint and by testing the same fixtures on both platforms; risk of drift between the platform layers — mitigated by integration tests that run identical scenarios on both OSes in CI.

## References

- `docs/architecture/storage-model.md`
- `docs/architecture/process-model.md`
- `docs/specifications/plugin-api.md`
- ADR-016 (testing strategy — the CI matrix)
- ADR-022 (toolchain bootstrapping)
- ADR-019 (Eigen adoption — header-only linear algebra portability)
