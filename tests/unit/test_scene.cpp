#include <gtest/gtest.h>

#include <string>

#include "core/scene/capture_session.h"
#include "core/scene/frame.h"
#include "core/scene/identity.h"
#include "core/scene/observation_graph/image_observation.h"
#include "core/scene/scene.h"
#include "core/utils/uuid.h"

namespace spatial::core {
namespace {

TEST(SceneIdentity, FrameIdDeterministic) {
  const Uuid sensor = ParseUuid("11111111-1111-4111-8111-111111111111");
  const std::string hash =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  EXPECT_EQ(DeriveFrameId(sensor, 123, hash), DeriveFrameId(sensor, 123, hash));
  EXPECT_NE(DeriveFrameId(sensor, 123, hash),
            DeriveFrameId(sensor, 124, hash));
  EXPECT_NE(DeriveFrameId(sensor, 123, hash),
            DeriveFrameId(ParseUuid("22222222-2222-4222-8222-222222222222"),
                          123, hash));
}

TEST(SceneIdentity, ObservationIdVsFrameId) {
  const Uuid sensor = ParseUuid("11111111-1111-4111-8111-111111111111");
  const std::string hash =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  EXPECT_NE(DeriveObservationId(sensor, 5, hash),
            DeriveFrameId(sensor, 5, hash));
}

TEST(SceneIdentity, CanonicalNameShape) {
  const Uuid sensor = ParseUuid("11111111-1111-4111-8111-111111111111");
  const std::string hash = std::string(64, 'a');
  const std::string name =
      EntityIdentityName("frame", sensor, 42, hash);
  EXPECT_EQ(name,
            "frame|11111111-1111-4111-8111-111111111111|42|" + hash);
}

TEST(SceneRecords, DefaultsAreSane) {
  CaptureSession session;
  EXPECT_TRUE(IsNil(session.session_id));
  EXPECT_EQ(session.status, "open");

  Frame frame;
  EXPECT_TRUE(IsNil(frame.frame_id));
  EXPECT_TRUE(IsNil(frame.pose_ref));
  EXPECT_EQ(frame.sequence_index, 0);

  ImageObservation obs;
  EXPECT_TRUE(IsNil(obs.observation_id));
  EXPECT_TRUE(IsNil(obs.artifact_ref));
  EXPECT_EQ(obs.width, 0);
  EXPECT_FALSE(obs.focal_prior_px.has_value());

  Scene scene;
  EXPECT_EQ(scene.stage, "created");
  EXPECT_EQ(scene.status, "open");

  SceneVersion version;
  EXPECT_EQ(version.stage, "");
  EXPECT_EQ(version.status, "active");
}

}  // namespace
}  // namespace spatial::core
