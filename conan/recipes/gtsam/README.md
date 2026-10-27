# GTSAM 4.2.1 Windows build compatibility

This is a copy of the ConanCenterIndex recipe with upstream recipe revision
`e9afbb277c20b9f6340caf83c70ea652`, plus one registered build-only patch. The
recipe and original packaging patches are MIT licensed; see `LICENSE-CII.txt`.
GTSAM source remains BSD licensed and is downloaded from the archive whose
SHA-256 is pinned in `conandata.yml`.

The local patch removes `#include <gtsam/base/chartTesting.h>` from
`gtsam/precompiled_header.h`. That obsolete test helper refers to the removed
`DefaultChart` type and breaks the MSVC precompiled-header build. It changes the internal build-time header list; numerical sources and
canonical platform APIs are unchanged. Upstream's two original packaging
patches are retained. Algorithm sources are unchanged.

Both platforms build the dependency itself as C++17 to accommodate Spectra 0.9
bundled in GTSAM 4.2.1; the platform and adapters remain C++20. Windows keeps its
existing TBB/METIS/unstable-module options. Linux uses the original locked CCI
recipe and its serial/stable options.

Before a Windows dependency install, export the recipe:

```sh
conan export conan/recipes/gtsam --version=4.2.1 --user=spatial --channel=compat
```

Windows profiles use `[replace_requires]` to select this namespaced reference.
`conan.lock` pins its exact recipe revision separately from the original Linux
reference. A recipe or patch change requires an explicit lockfile update.
`.gitattributes` fixes LF for recipe data and patches so the revision is stable
on Windows and Linux. No binary is relabeled as this recipe.
