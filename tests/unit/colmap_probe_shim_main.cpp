#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// COLMAP CLI stand-in (test helper, C1-S2 doctor / C1-S3 execution).
// Replaces a COLMAP installation in the adapter tests: `--version` answers the
// doctor probe; the stage subcommands (feature_extractor / exhaustive_matcher
// / mapper) behave like the real tools well enough to exercise the adapter's
// process lifecycle — they emit stdout/stderr, honor the workspace layout,
// and write the produced sparse-model payload.
//
// Test-only behavior, triggered from the workspace (the child's working
// directory):
//   * a file named `shim_fail` -> print an error to stderr and exit 42;
//   * a file named `shim_hang` -> sleep 30 s (used to prove timeout and
//     cancellation terminate the process).
// Every invocation appends its argv to `<cwd>/logs/args.txt` so the tests can
// assert the adapter passed local workspace paths and never a CAS hash.

namespace {

void DumpArgv(const std::vector<std::string>& argv) {
  const std::filesystem::path cwd = std::filesystem::current_path();
  std::error_code ec;
  std::filesystem::create_directories(cwd / "logs", ec);
  std::ofstream out(cwd / "logs" / "args.txt", std::ios::app);
  if (!out) {
    return;
  }
  out << "argv:";
  for (const auto& arg : argv) {
    out << '|' << arg;
  }
  out << '\n';
}

bool HasMarker(const std::string& name) {
  return std::filesystem::exists(std::filesystem::current_path() / name);
}

std::string FlagValue(const std::vector<std::string>& argv,
                      const std::string& flag) {
  for (std::size_t i = 1; i + 1 < argv.size(); ++i) {
    if (argv[i] == flag) {
      return argv[i + 1];
    }
  }
  return "";
}

int RunFeatureExtractor(const std::vector<std::string>& argv) {
  const std::string image_path = FlagValue(argv, "--image_path");
  if (image_path.empty() ||
      !std::filesystem::is_directory(image_path)) {
    std::fprintf(stderr,
                 "feature_extractor: --image_path does not exist: %s\n",
                 image_path.c_str());
    return 66;
  }
  std::puts((std::string("feature_extractor: reading images from ") +
             image_path)
                .c_str());
  std::puts("feature_extractor: extracting SIFT features");
  std::fputs("feature_extractor: database opened\n", stderr);
  return 0;
}

int RunMatcher() {
  std::puts("exhaustive_matcher: matching 2 image pairs");
  std::fputs("exhaustive_matcher: wrote matches\n", stderr);
  return 0;
}

int RunMapper(const std::vector<std::string>& argv) {
  const std::string output_path = FlagValue(argv, "--output_path");
  if (output_path.empty()) {
    std::fputs("mapper: missing --output_path\n", stderr);
    return 66;
  }
  std::puts("mapper: running bundle adjustment");
  const std::filesystem::path model_dir = output_path;
  std::error_code ec;
  std::filesystem::create_directories(model_dir / "0", ec);
  if (ec) {
    std::fprintf(stderr, "mapper: cannot create %s\n",
                 (model_dir / "0").string().c_str());
    return 66;
  }
  std::ofstream out(model_dir / "0" / "model.bin", std::ios::binary);
  out << "sparse-model payload v0.1\n";
  std::puts("mapper: wrote sparse/0/model.bin");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  if (args.size() >= 2 && args[1] == "--version") {
    std::puts("COLMAP probe shim 0.1.0 (test helper)");
    std::fputs("shim: --version ok\n", stderr);
    return 0;
  }

  if (args.size() < 2) {
    std::fputs("usage: colmap <command> [options]\n", stderr);
    return 64;
  }

  DumpArgv(args);

  if (HasMarker("shim_fail")) {
    std::fputs("shim: failure marker triggered\n", stderr);
    return 42;
  }
  if (HasMarker("shim_hang")) {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 0;
  }

  const std::string& command = args[1];
  if (command == "feature_extractor") {
    return RunFeatureExtractor(args);
  }
  if (command == "exhaustive_matcher") {
    return RunMatcher();
  }
  if (command == "mapper") {
    return RunMapper(args);
  }
  std::fprintf(stderr, "colmap: unknown command '%s'\n", command.c_str());
  return 64;
}
