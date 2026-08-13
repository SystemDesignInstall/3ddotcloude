#include <cstdio>

// COLMAP probe shim (test helper, C1-S2). ValidateEnvironment probes the
// adapter's configured executable with `--version`; this shim stands in for a
// runnable COLMAP installation in the doctor test (present case) by answering
// the probe with exit code 0. The missing-executable case needs no helper:
// an unresolvable binary makes the probe return non-zero.

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  std::puts("COLMAP probe shim 0.1.0 (test helper)");
  return 0;
}
