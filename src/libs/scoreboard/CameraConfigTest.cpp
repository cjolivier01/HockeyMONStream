#include "hstream/src/libs/scoreboard/CameraConfig.h"
#include "hstream/src/libs/media/TestMp4.h"

namespace {
using hm::media::test::Check;
}
int main(int argc, char** argv) {
  try {
    gst_init(&argc, &argv);
    using hm::scoreboard::ParseCameraStartOffset;
    Check(!ParseCameraStartOffset(std::string(10000, '9') + ":00:00.0000").ok(), "Unbounded offset accepted");
    Check(*ParseCameraStartOffset("-00:00:05.0000") == -5000000000LL, "Negative delay lost its sign");
    Check(*ParseCameraStartOffset("+01:02:03.4567") == 3723456700000LL, "Offset lost sub-millisecond precision");
    for (const auto* invalid :
         {"",
          "5",
          "00:60:00.0000",
          "00:00:60.0000",
          "00:00:00.00001",
          "00:00:00.0000x",
          "-00:00:00",
          "999999999999999999999:00:00.0000"})
      Check(!ParseCameraStartOffset(invalid).ok(), "Invalid offset accepted");
    Check(argc == 2, "Expected baseline path");
    const auto baseline = YAML::LoadFile(argv[1])["rink"]["scoreboard"]["camera"];
    const auto resolve = [&](const YAML::Node& node, const std::filesystem::path& root = {}) {
      return hm::scoreboard::ResolveCameraConfig(node, root, hm::scoreboard::CameraDecodeBackend::kNvidia, 0, {});
    };
    auto disabled = resolve(baseline);
    Check(disabled.ok() && !*disabled, "Baseline unexpectedly enables a scoreboard decoder");
    auto live = YAML::Clone(baseline);
    live["enabled"] = true;
    live["uri"] = "rtsp://127.0.0.1/scoreboard";
    live["codec"] = "h264";
    live["source_dimensions"] = YAML::Load("[384, 192]");
    live["polygon"] = YAML::Load("[[96,40], [223,40], [223,103], [96,103]]");
    auto result = resolve(live);
    Check(result.ok() && result->has_value(), result.status().ToString());
    Check(
        (*result)->decoder.region.left == 96 && (*result)->decoder.region.width == 128,
        "Camera ROI did not retain source coordinates");
    Check((*result)->decoder.queued_frames == baseline["queue_frames"].as<unsigned>(), "Queue ignored baseline");
    Check(
        (*result)->decoder.operation_timeout == baseline["operation_timeout_ms"].as<uint64_t>() * GST_MSECOND,
        "Decoder deadline ignored baseline");
    for (const char* key :
         {"enabled",
          "source_dimensions",
          "polygon",
          "queue_frames",
          "extra_decode_surfaces",
          "operation_timeout_ms",
          "cancellation_poll_ms",
          "start_offset",
          "live_latency_ms",
          "codec"}) {
      auto missing = YAML::Clone(live);
      missing.remove(key);
      Check(!resolve(missing).ok(), std::string("Missing camera field silently defaulted: ") + key);
    }
    for (const char* key : {"queue_frames", "operation_timeout_ms", "cancellation_poll_ms"}) {
      auto invalid = YAML::Clone(live);
      invalid[key] = 0;
      Check(!resolve(invalid).ok(), std::string("Zero camera bound accepted: ") + key);
    }
    auto invalid = YAML::Clone(live);
    invalid["files"].push_back("recording.mp4");
    Check(!resolve(invalid).ok(), "Recorded/live source ambiguity accepted");
    invalid = YAML::Clone(live);
    invalid["uri"] = "file:///tmp/recording.mp4";
    Check(!resolve(invalid).ok(), "Live source silently used a file");
    hm::media::test::Directory directory;
    hm::media::test::Chapter(directory.path / "camera.mp4", "red", "0");
    auto recorded = YAML::Clone(live);
    recorded["uri"] = YAML::Node(YAML::NodeType::Null);
    recorded["codec"] = YAML::Node(YAML::NodeType::Null);
    recorded["files"].push_back("camera.mp4");
    recorded["files"].push_back("camera.mp4");
    recorded["start_offset"] = "00:00:00.0001";
    result = resolve(recorded, directory.path);
    Check(result.ok() && result->has_value(), result.status().ToString());
    Check(
        (*result)->chapters.size() == 2 && (*result)->chapters[1].start_ns == 200 * GST_MSECOND,
        "Chapter timing included audio tail");
    Check((*result)->offset_ns == -100000 && !(*result)->decoder.live, "Recorded source lost synchronization");
    recorded["source_dimensions"][0] = 386;
    Check(!resolve(recorded, directory.path).ok(), "Changed recording geometry reused saved crop");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
