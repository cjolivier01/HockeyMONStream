#include "hstream/src/gst-plugins/testutils/GstPluginTestHarness.h"
#include "hstream/src/libs/media/TestMp4.h"
#include "src/apps/hstream-cli/configurator.h"
#include "src/apps/hstream-cli/deepstream_app.h"

#include <memory>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace hm {
struct ConfiguratorTestAccess {
  static void Enable(Configurator& value, bool live = false) {
    value.config_["rink"]["scoreboard"]["camera"]["enabled"] = true;
    value.config_["rink"]["scoreboard"]["camera"]["uri"] =
        live ? YAML::Node("rtsp://scoreboard/live") : YAML::Node(YAML::NodeType::Null);
  }
  static absl::Status Rates(
      Configurator& value,
      const std::vector<std::string>& left,
      const std::vector<std::string>& right,
      YAML::Node& pipeline) {
    size_t width = 0, height = 0, area = 0;
    return value.apply_frame_offsets_and_sizes(
        left, right, YAML::Load("{left: 0, right: 0}"), width, height, area, pipeline);
  }
};
} // namespace hm
int main() {
  using hm::media::test::Check;
  gst_init(nullptr, nullptr);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  try {
    Check(
        hm::gst::test::load_plugin_from_runfiles("src/gst-plugins/gst-videoprep/libnvdsgst_videoprep.so"),
        "Cannot load cropper plugin");
    hm::media::test::Directory directory;
    for (const int fps : {30, 60})
      hm::media::test::Ffmpeg(
          {"-f",
           "lavfi",
           "-i",
           "testsrc2=s=384x192:r=" + std::to_string(fps) + ":d=0.2",
           "-c:v",
           "libx264",
           (directory.path / (std::to_string(fps) + ".mp4")).string()});
    hm::Configurator configurator("scoreboard-clock", "configs", -1);
    for (const bool live : {false, true}) {
      hm::ConfiguratorTestAccess::Enable(configurator, live);
      auto pipeline = std::make_unique<NvDsPipeline>();
      auto config = std::make_unique<NvDsConfig>();
      const auto status = configurator.prepare_initial_pipeline_position(*pipeline, *config, 0);
      Check(status.ok(), "A stage without a scoreboard compositor must ignore scoreboard clock setup");
    }
    hm::ConfiguratorTestAccess::Enable(configurator);
    for (int count : {1, 2}) {
      for (const bool mixed : {false, true}) {
        if (count == 1 && mixed)
          continue;
        auto pipeline = std::make_unique<NvDsPipeline>();
        auto config = std::make_unique<NvDsConfig>();
        auto* cropper = gst_element_factory_make("playcropper", nullptr);
        Check(cropper, "Cannot create cropper");
        pipeline->common_elements.hmplaycropper_bin.playcropper = cropper;
        const auto left = directory.path / "60.mp4";
        const auto right = directory.path / (mixed ? "30.mp4" : "60.mp4");
        gchar* uri = g_filename_to_uri(left.c_str(), nullptr, nullptr);
        config->multi_source_config[0].uri = uri;
        if (count == 2) {
          YAML::Node materialized;
          const auto status =
              hm::ConfiguratorTestAccess::Rates(configurator, {left.string()}, {right.string()}, materialized);
          Check(status.ok(), status.ToString());
          for (int index = 0; index < count; ++index) {
            auto source = materialized["source" + std::to_string(index)];
            config->multi_source_config[index].camera_fps_n = source["camera-fps-n"].as<int>();
            config->multi_source_config[index].camera_fps_d = source["camera-fps-d"].as<int>();
          }
        }
        // Before preroll, the single-source case deliberately has no decoded
        // caps or injected rate; the equal-rate pair uses normal preparation.
        const auto status = configurator.prepare_initial_pipeline_position(*pipeline, *config, 0);
        Check(status.ok(), status.ToString());
        gchar* properties = nullptr;
        g_object_get(cropper, "plugin-private-config", &properties, nullptr);
        Check(
            properties &&
                std::string(properties).find(mixed ? "timeline=0,30,1" : "timeline=0,60,1") != std::string::npos,
            "Initial scoreboard clock did not follow the game cameras");
        g_free(properties);
        g_free(uri);
        gst_object_unref(cropper);
      }
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
