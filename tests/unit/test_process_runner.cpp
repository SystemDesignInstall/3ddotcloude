// Generic adapter-layer subprocess runner tests (C1-S3, plan §9/§11
// test_process_runner). Drives real child processes (the COLMAP probe shim)
// through RunSubprocess and asserts the lifecycle contract: output capture,
// exit-code propagation, working-directory honor, spawn failures, and the
// mandatory terminate-and-reap behavior on timeout and cooperative
// cancellation (no orphan process is ever left behind).

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "adapters/process/process_runner.h"

namespace spatial::adapters::process {
namespace {

#ifndef SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE
#error SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE must be defined by the test build
#endif

// Scratch working directory for the spawned children (the shim writes its
// argv dump and the mapper payload into the child's cwd).
class TempDir {
 public:
  TempDir() {
    path_ = std::filesystem::temp_directory_path() /
            ("spatial_proc_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::filesystem::path path_;
};

ProcessSpec ShimSpec(const std::filesystem::path& cwd,
                     std::vector<std::string> argv) {
  ProcessSpec spec;
  spec.argv = std::move(argv);
  spec.working_directory = cwd;
  return spec;
}

TEST(ProcessRunnerTest, CapturesRealChildOutput) {
  TempDir tmp;
  const ProcessResult result = RunSubprocess(
      ShimSpec(tmp.path_, {SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, "--version"}),
      10000);
  EXPECT_EQ(result.outcome, ProcessOutcome::kCompleted);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_NE(result.stdout_text.find("probe shim"), std::string::npos);
  EXPECT_NE(result.stderr_text.find("--version ok"), std::string::npos);
  EXPECT_TRUE(result.error_message.empty());
}

TEST(ProcessRunnerTest, PropagatesNonZeroExitCode) {
  TempDir tmp;
  const ProcessResult result = RunSubprocess(
      ShimSpec(tmp.path_, {SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE,
                           "not_a_command"}),
      10000);
  EXPECT_EQ(result.outcome, ProcessOutcome::kCompleted);
  EXPECT_EQ(result.exit_code, 64);
  EXPECT_NE(result.stderr_text.find("unknown command"), std::string::npos);
}

TEST(ProcessRunnerTest, HonorsWorkingDirectory) {
  TempDir tmp;
  const ProcessResult result = RunSubprocess(
      ShimSpec(tmp.path_, {SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, "mapper",
                           "--output_path", tmp.path_.string()}),
      10000);
  EXPECT_EQ(result.outcome, ProcessOutcome::kCompleted);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(std::filesystem::exists(tmp.path_ / "0" / "model.bin"));
  EXPECT_TRUE(std::filesystem::exists(tmp.path_ / "logs" / "args.txt"));
}

TEST(ProcessRunnerTest, SeparatesStdoutAndStderr) {
  TempDir tmp;
  const ProcessResult result = RunSubprocess(
      ShimSpec(tmp.path_, {SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, "mapper",
                           "--output_path", tmp.path_.string()}),
      10000);
  EXPECT_EQ(result.outcome, ProcessOutcome::kCompleted);
  EXPECT_NE(result.stdout_text.find("running bundle adjustment"),
            std::string::npos);
  EXPECT_EQ(result.stderr_text.find("running bundle adjustment"),
            std::string::npos);
}

TEST(ProcessRunnerTest, SpawnFailureIsReported) {
  TempDir tmp;
  const ProcessResult result =
      RunSubprocess(ShimSpec(tmp.path_, {"spatial-nonexistent-binary-xyz", "x"}),
                    1000);
  EXPECT_EQ(result.outcome, ProcessOutcome::kSpawnFailed);
  EXPECT_FALSE(result.error_message.empty());
}

TEST(ProcessRunnerTest, TimesOutAndTerminatesChild) {
  TempDir tmp;
  std::ofstream(tmp.path_ / "shim_hang").close();
  const auto start = std::chrono::steady_clock::now();
  const ProcessResult result = RunSubprocess(
      ShimSpec(tmp.path_, {SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, "mapper",
                           "--output_path", tmp.path_.string()}),
      400);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  EXPECT_EQ(result.outcome, ProcessOutcome::kTimedOut);
  EXPECT_EQ(result.exit_code, -1);
  EXPECT_LT(elapsed.count(), 3000)
      << "the hung child must be terminated at the deadline";
}

TEST(ProcessRunnerTest, CancellationTerminatesChild) {
  TempDir tmp;
  std::ofstream(tmp.path_ / "shim_hang").close();
  CancelToken token;
  std::thread canceller([&token] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    token.Cancel();
  });
  const ProcessResult result = RunSubprocess(
      ShimSpec(tmp.path_, {SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, "mapper",
                           "--output_path", tmp.path_.string()}),
      10000, &token);
  canceller.join();
  EXPECT_EQ(result.outcome, ProcessOutcome::kCancelled);
  EXPECT_EQ(result.exit_code, -1);
}

}  // namespace
}  // namespace spatial::adapters::process
