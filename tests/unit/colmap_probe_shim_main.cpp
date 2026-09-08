#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// COLMAP CLI stand-in (test helper, C1-S2 doctor / C1-S3 execution /
// C1-S4 worker E2E).
// Replaces a COLMAP installation in the adapter tests: `--version` answers the
// doctor probe; the stage subcommands (feature_extractor / exhaustive_matcher
// / mapper) behave like the real tools well enough to exercise the adapter's
// process lifecycle — they emit stdout/stderr, honor the workspace layout,
// and write the produced sparse-model payload.
//
// The mapper writes the three COLMAP native model files (cameras.bin /
// images.bin / points3D.bin) in the real little-endian binary layout
// documented in adapters/colmap/colmap_converter.h, so the full
// worker -> adapter -> converter -> canonical artifact chain runs with no
// COLMAP install. The model is a deterministic function of the sorted image
// file list (equal inputs -> equal bytes, ADR-020).
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

// Little-endian binary writers (COLMAP writes ReadBinaryLittleEndian).
class BinWriter {
 public:
  explicit BinWriter(const std::filesystem::path& path) : out_(path,
                                                                  std::ios::binary) {}
  bool ok() const { return static_cast<bool>(out_); }
  void U8(std::uint8_t value) { out_.put(static_cast<char>(value)); }
  void U32(std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      out_.put(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
  }
  void I32(std::int32_t value) { U32(static_cast<std::uint32_t>(value)); }
  void I64(std::int64_t value) { U64(static_cast<std::uint64_t>(value)); }
  void U64(std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      out_.put(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
  }
  void F64(double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "IEEE-754 double expected");
    std::memcpy(&bits, &value, sizeof(bits));
    U64(bits);
  }
  void String(const std::string& value) {
    U32(static_cast<std::uint32_t>(value.size()));
    out_.write(value.data(), static_cast<std::streamsize>(value.size()));
  }

 private:
  std::ofstream out_;
};

// Writes a deterministic single-camera sparse model into `model_dir`
// (cameras.bin / images.bin / points3D.bin) from the sorted image file list.
// Camera: SIMPLE_PINHOLE, 640x480, f=1000, c=(320,240). One image per file
// (identity pose, one observed point), one 3D point tracked by every image.
void WriteSparseModel(const std::filesystem::path& model_dir,
                      const std::vector<std::filesystem::path>& image_files) {
  // cameras.bin: 1 camera (model id 0 = SIMPLE_PINHOLE).
  {
    BinWriter out(model_dir / "cameras.bin");
    out.U64(1);
    out.U32(1);              // camera_id
    out.I32(0);              // SIMPLE_PINHOLE
    out.U64(640);            // width
    out.U64(480);            // height
    out.F64(1000.0);         // f
    out.F64(320.0);          // cx
    out.F64(240.0);          // cy
  }
  // images.bin: one image per file, id = index + 1.
  {
    BinWriter out(model_dir / "images.bin");
    out.U64(image_files.size());
    for (std::size_t i = 0; i < image_files.size(); ++i) {
      out.U32(static_cast<std::uint32_t>(i + 1));  // image_id
      out.F64(0.0);                                // qw
      out.F64(0.0);                                // qx
      out.F64(0.0);                                // qy
      out.F64(1.0);                                // qz
      out.F64(0.1 * static_cast<double>(i));       // tx
      out.F64(0.0);                                // ty
      out.F64(0.0);                                // tz
      out.U32(1);                                  // camera_id
      out.String(image_files[i].filename().string());
      out.U64(1);                                  // num_points2D
      out.F64(320.5);                              // x
      out.F64(240.25);                             // y
      out.I64(1);                                  // point3D_id
    }
  }
  // points3D.bin: one point observed by every image.
  {
    BinWriter out(model_dir / "points3D.bin");
    out.U64(1);
    out.U64(1);              // point3D_id
    out.F64(1.0);            // x
    out.F64(2.0);            // y
    out.F64(3.0);            // z
    out.U8(200);             // r
    out.U8(100);             // g
    out.U8(50);              // b
    out.F64(0.5);            // error
    out.U64(image_files.size());
    for (std::size_t i = 0; i < image_files.size(); ++i) {
      out.U32(static_cast<std::uint32_t>(i + 1));  // image_id
      out.U32(0);                                  // point2D_idx
    }
  }
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
  std::vector<std::filesystem::path> image_files;
  if (std::filesystem::is_directory("images")) {
    for (const auto& entry :
         std::filesystem::directory_iterator("images")) {
      if (entry.is_regular_file()) {
        image_files.push_back(entry.path());
      }
    }
  }
  std::sort(image_files.begin(), image_files.end());
  WriteSparseModel(model_dir / "0", image_files);
  std::puts("mapper: wrote sparse/0 model (cameras.bin, images.bin, "
            "points3D.bin)");
  return 0;
}

std::uint64_t ReadU64(std::ifstream& in) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    const int byte = in.get();
    if (byte < 0) return 0;
    value |= static_cast<std::uint64_t>(byte) << (8 * i);
  }
  return value;
}

std::uint32_t ReadU32(std::ifstream& in) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    const int byte = in.get();
    if (byte < 0) return 0;
    value |= static_cast<std::uint32_t>(byte) << (8 * i);
  }
  return value;
}

double ReadF64(std::ifstream& in) {
  std::uint64_t bits = ReadU64(in);
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// The bundle_adjuster stand-in (P3-impl-8c): reads the native input model the
// adapter wrote under --input_path (sparse/0) and emits a deterministic
// "refined" model under --output_path (sparse_ba), matching the real tool's
// fixed-intrinsics contract: cameras.bin and images.bin pass through VERBATIM
// (intrinsics + poses untouched), points3D.bin is rebuilt with every point's x
// coordinate offset by +0.5 (a deterministic, reversible refinement so tests
// can prove the output model — and not a stale copy of the input — is what the
// adapter parses back).
int RunBundleAdjuster(const std::vector<std::string>& argv) {
  const std::string input_path = FlagValue(argv, "--input_path");
  const std::string output_path = FlagValue(argv, "--output_path");
  if (input_path.empty() || !std::filesystem::is_directory(input_path)) {
    std::fprintf(stderr,
                 "bundle_adjuster: --input_path does not exist: %s\n",
                 input_path.c_str());
    return 66;
  }
  if (output_path.empty()) {
    std::fputs("bundle_adjuster: missing --output_path\n", stderr);
    return 66;
  }
  const std::filesystem::path in_dir = input_path;
  const std::filesystem::path out_dir = output_path;
  // Test-only: prove the adapter fails closed (P12-iii) on a bundle_adjuster
  // that exits 0 but writes no refined model.
  if (HasMarker("shim_bundle_adjuster_no_output")) {
    std::puts("bundle_adjuster: (shim) ran without writing a model");
    return 0;
  }
  for (const char* file : {"cameras.bin", "images.bin", "points3D.bin"}) {
    if (!std::filesystem::exists(in_dir / file)) {
      std::fprintf(stderr, "bundle_adjuster: input %s missing\n", file);
      return 66;
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    std::fprintf(stderr, "bundle_adjuster: cannot create %s\n",
                 out_dir.string().c_str());
    return 66;
  }
  std::filesystem::copy_file(in_dir / "cameras.bin", out_dir / "cameras.bin",
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  if (ec) {
    std::fprintf(stderr, "bundle_adjuster: cannot copy cameras.bin\n");
    return 66;
  }
  std::filesystem::copy_file(in_dir / "images.bin", out_dir / "images.bin",
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  if (ec) {
    std::fprintf(stderr, "bundle_adjuster: cannot copy images.bin\n");
    return 66;
  }
  const double kPointOffset = 0.5;
  {
    std::ifstream in(in_dir / "points3D.bin", std::ios::binary);
    BinWriter out(out_dir / "points3D.bin");
    const std::uint64_t num_points = ReadU64(in);
    out.U64(num_points);
    for (std::uint64_t i = 0; i < num_points; ++i) {
      out.U64(ReadU64(in));                                   // point3D_id
      const double x = ReadF64(in) + kPointOffset;            // refined x
      out.F64(x);
      out.F64(ReadF64(in));                                   // y
      out.F64(ReadF64(in));                                   // z
      out.U8(static_cast<std::uint8_t>(in.get()));            // r
      out.U8(static_cast<std::uint8_t>(in.get()));            // g
      out.U8(static_cast<std::uint8_t>(in.get()));            // b
      out.F64(ReadF64(in));                                   // error
      const std::uint64_t track_len = ReadU64(in);
      out.U64(track_len);
      for (std::uint64_t j = 0; j < track_len; ++j) {
        const std::uint32_t image_id = ReadU32(in);
        const std::uint32_t point2d_idx = ReadU32(in);
        out.U32(image_id);
        out.U32(point2d_idx);
      }
    }
  }
  std::puts("bundle_adjuster: refined model written to sparse_ba "
            "(poses + intrinsics fixed, points adjusted)");
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
  if (command == "bundle_adjuster") {
    return RunBundleAdjuster(args);
  }
  std::fprintf(stderr, "colmap: unknown command '%s'\n", command.c_str());
  return 64;
}
