#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/errors/project_error.h"
#include "engine/workers/child_process.h"
#include "engine/workers/protocol_framing.h"

#ifndef SPATIAL_FRAME_ECHO_EXECUTABLE
#error SPATIAL_FRAME_ECHO_EXECUTABLE must be defined by the test build
#endif

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::WorkerError;

std::string kEchoExe = SPATIAL_FRAME_ECHO_EXECUTABLE;

TEST(ProtocolFraming, RoundTripTaskRequest) {
  std::string error;
  auto child = ChildProcess::Spawn({kEchoExe}, error);
  ASSERT_NE(child, nullptr) << error;

  spatial::WorkerMessage msg;
  auto* req = msg.mutable_task_request();
  req->set_task_id("task-1");
  req->set_task_type("feature_extract");
  req->set_spec_json(R"({"threshold":0.5})");
  req->set_workspace("temp/job/task");
  ASSERT_TRUE(WriteFrame(*child, msg, error));

  std::string frame;
  bool eof = false;
  ASSERT_TRUE(ReadFrame(*child, 5000, frame, eof, error)) << error;
  spatial::WorkerMessage back;
  ASSERT_TRUE(TryParseFrame(frame, back));
  ASSERT_TRUE(back.has_task_request());
  EXPECT_EQ(back.task_request().task_id(), "task-1");
  EXPECT_EQ(back.task_request().task_type(), "feature_extract");
  EXPECT_EQ(back.task_request().spec_json(), R"({"threshold":0.5})");
  EXPECT_EQ(back.task_request().workspace(), "temp/job/task");
}

TEST(ProtocolFraming, RoundTripProgressInOrder) {
  std::string error;
  auto child = ChildProcess::Spawn({kEchoExe}, error);
  ASSERT_NE(child, nullptr) << error;

  spatial::WorkerMessage a;
  a.mutable_task_progress()->set_task_id("t");
  a.mutable_task_progress()->set_percent(42);
  a.mutable_task_progress()->set_substage("execute");
  spatial::WorkerMessage b;
  b.mutable_heartbeat()->set_timestamp_ns(12345);

  ASSERT_TRUE(WriteFrame(*child, a, error));
  ASSERT_TRUE(WriteFrame(*child, b, error));

  std::string frame;
  bool eof = false;
  ASSERT_TRUE(ReadFrame(*child, 5000, frame, eof, error)) << error;
  spatial::WorkerMessage back_a;
  ASSERT_TRUE(TryParseFrame(frame, back_a));
  ASSERT_TRUE(back_a.has_task_progress());
  EXPECT_EQ(back_a.task_progress().percent(), 42);

  ASSERT_TRUE(ReadFrame(*child, 5000, frame, eof, error)) << error;
  spatial::WorkerMessage back_b;
  ASSERT_TRUE(TryParseFrame(frame, back_b));
  ASSERT_TRUE(back_b.has_heartbeat());
  EXPECT_EQ(back_b.heartbeat().timestamp_ns(), 12345);
}

TEST(ProtocolFraming, ReadFrameTimesOutWithoutData) {
  std::string error;
  auto child = ChildProcess::Spawn({kEchoExe}, error);
  ASSERT_NE(child, nullptr) << error;

  std::string frame;
  bool eof = false;
  EXPECT_FALSE(ReadFrame(*child, 200, frame, eof, error));
  EXPECT_FALSE(eof);
  child->Terminate();
  child->Wait();
}

TEST(ProtocolFraming, EofAfterChildExit) {
  std::string error;
  auto child = ChildProcess::Spawn({kEchoExe}, error);
  ASSERT_NE(child, nullptr) << error;
  child->Terminate();
  child->Wait();

  std::string frame;
  bool eof = false;
  EXPECT_FALSE(ReadFrame(*child, 500, frame, eof, error));
  EXPECT_TRUE(eof);
}

TEST(ProtocolFraming, RejectsMalformedPayload) {
  spatial::WorkerMessage msg;
  EXPECT_FALSE(TryParseFrame("garbage-bytes-that-are-not-proto", msg));
  // A frame with no payload at all is malformed too.
  EXPECT_FALSE(TryParseFrame("", msg));
}

TEST(ProtocolFraming, SpawnMissingProgramFails) {
  std::string error;
  auto child = ChildProcess::Spawn({"definitely-not-a-real-program-xyz"}, error);
  EXPECT_EQ(child, nullptr);
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace spatial::engine
