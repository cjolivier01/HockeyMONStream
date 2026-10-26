#include "src/apps/hstream-cli/configurator.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <gst/gst.h>
#include <opencv2/imgcodecs.hpp>
#include <tiffio.h>
#include <unistd.h>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/RinkMaskInputStore.h"

GST_DEBUG_CATEGORY(NVDS_APP);
namespace hm {
struct ConfiguratorTestAccess {
  static void isolate_user_config(Configurator& config) {
    config.user_config_snapshot_ = YAML::Node(YAML::NodeType::Map);
  }
};
} // namespace hm
namespace {
namespace fs = std::filesystem;
using namespace hm::stitching;
void require(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
void require(const absl::Status& status) {
  require(status.ok(), status.ToString());
}
template <class T>
T value(absl::StatusOr<T> status) {
  require(status.status());
  return std::move(*status);
}
void mapping(const fs::path& path) {
  TIFF* tiff = TIFFOpen(path.c_str(), "w");
  require(tiff != nullptr, "create mapping");
  TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, 32);
  TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, 24);
  TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tiff, TIFFTAG_XRESOLUTION, 1.0f);
  TIFFSetField(tiff, TIFFTAG_YRESOLUTION, 1.0f);
  TIFFSetField(tiff, TIFFTAG_XPOSITION, 0.0f);
  TIFFSetField(tiff, TIFFTAG_YPOSITION, 0.0f);
  uint16_t row[32]{};
  for (int y = 0; y < 24; ++y)
    require(TIFFWriteScanline(tiff, row, y, 0) >= 0, "mapping row");
  TIFFClose(tiff);
}
void fixture(
    const fs::path& directory,
    const YAML::Node& baseline,
    const fs::path& video,
    const std::string& format,
    const std::string& mode,
    bool configure_only) {
  fs::create_directory(directory);
  YAML::Node config = YAML::Clone(baseline);
  config["pipeline"]["application"]["complete-configuration"] = 1;
  config["pipeline"]["application"]["gpu-memory-profile"] = "normal";
  config["pipeline"]["hmstitcher"]["enable"] = 1;
  config["pipeline"]["hmstitcher"]["one-pass-mode"] = configure_only ? 0 : 1;
  config["pipeline"]["hmstitcher"]["configure-only"] = configure_only ? 1 : 0;
  config["pipeline"]["hmstitcher"]["properties"]["high-bit-depth"] = mode;
  config["pipeline"]["hmstitcher"]["private-properties"]["calibrate-field-mask"] = true;
  config["pipeline"]["ds-playtracker"]["enable"] = 0;
  config["pipeline"]["hmplaycropper"]["enable"] = 0;
  for (int camera = 0; camera < 2; ++camera) {
    auto source = config["pipeline"]["source" + std::to_string(camera)];
    source["enable"] = 1;
    source["type"] = static_cast<int>(NV_DS_SOURCE_URI_MULTIPLE);
    source["source-id"] = camera;
  }
  config["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  config["stitching"]["control_point_resolution"] = "native";
  write_control_point_filtering(config, {false, false});
  write_stitch_projection_framing(config, {});
  config["stitching"]["rink_mask_frame_time"] = "00:00:01";
  config["stitching"]["max_output_width"] = 0;
  config["stitching"]["max_output_dimension"] = 0;
  write_stitch_camera_selection(config, {"manual", 108, 90});
  config["stitching"]["camera_automation"]["fov"] = false;
  config["stitching"]["camera_automation"]["lens"] = false;
  config["game"]["videos"]["left"].push_back(video.string());
  config["game"]["videos"]["right"].push_back(video.string());
  config["game"]["stitching"]["frame_offsets"]["left"] = 0;
  config["game"]["stitching"]["frame_offsets"]["right"] = 0;
  config["hstream_ui"]["stitching_calibration"]["status"] = "complete";
  config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "precision-owner";
  config["hstream_ui"]["stitching_calibration"]["control_points"] = 1500;
  config["hstream_ui"]["stitching_calibration"]["frame_count"] = 4;
  std::ofstream(directory / "config.yaml") << config;
  for (const char* name : {"left.png", "right.png"})
    require(cv::imwrite((directory / name).string(), cv::Mat(24, 32, CV_8UC3, cv::Scalar(64, 64, 64))), "source image");
  for (const char* name : {"hm_project.pto", "autooptimiser_out.pto"})
    std::ofstream(directory / name) << "p f2 w32 h24\n";
  for (const char* name :
       {"mapping_0000.tif",
        "mapping_0001.tif",
        "mapping_0000_x.tif",
        "mapping_0000_y.tif",
        "mapping_0001_x.tif",
        "mapping_0001_y.tif"})
    mapping(directory / name);
  cv::Mat seam(24, 32, CV_8U, cv::Scalar(0));
  seam.colRange(16, 32).setTo(255);
  require(cv::imwrite((directory / "seam_file.png").string(), seam), "seam");
  const auto projection = value(ParseStitchProjection(config["stitching"]["projection"].as<std::string>()));
  const auto parameters =
      FormatStitchProjectionParameters(value(read_stitch_projection_parameters(config, projection)));
  std::ofstream(directory / kStitchCanvasProvenanceArtifact)
      << "version=14\nmax-output-width=0\nmax-canvas-dimension=0\nsource-canvas-width=32\nsource-canvas-height=24\n"
         "canvas-width=32\ncanvas-height=24\nmax-output-width-applied=0\nmax-canvas-dimension-applied=0\n"
         "mapping-backend=nona\nprojection="
      << StitchProjectionName(projection) << "\nprojection-parameters=" << (parameters.empty() ? "none" : parameters)
      << "\nprojection-auto-fov=0\nprojection-horizontal-fov=180\nprojection-auto-canvas=1\nprojection-auto-crop=0\n"
         "camera-configuration=manual\ncamera-horizontal-fov=108\ncamera-vertical-fov=90\n"
         "control-point-matcher=superpoint-lightglue\nakaze-calibration-fingerprint=not-applicable\n"
         "projection-rotation-0=0\nprojection-rotation-1=0\nprojection-rotation-2=0\n"
         "projection-crop-0=0\nprojection-crop-1=1\nprojection-crop-2=0\nprojection-crop-3=1\n"
         "control-point-resolution=native\ncalibration-frame-selection=none\ncalibration-frame-diagnostics=none\ninput-lens="
      << SerializeStitchInputLens(value(read_stitch_camera_selection(config)).input_lens)
      << "\ncontrol-point-filtering=0,0\n";
  auto lock = value(HuginProject::RecoverAndLock(directory));
  const auto generation = value(HuginProject::GenerationId(directory, *lock));
  const auto identity = value(stitch_artifact_content_identity_locked(directory));
  lock.reset();
  RinkMaskInputDescription description;
  description.width = 32;
  description.height = 24;
  description.format = format;
  description.hugin_generation = generation;
  description.hugin_fingerprint = identity.fingerprint;
  description.frame.selection = "00:00:01";
  description.frame.recording_time_ns = 1000000000ULL;
  description.frame.sources = {{{video, 1.0}, {video, 1.0}}};
  auto input = value(PublishRinkMaskInput(
      directory, description, value(ReadRinkMaskInputLimits(config)), [](uint32_t, void* row, size_t bytes) {
        std::memset(row, 0, bytes);
        return absl::OkStatus();
      }));
  description.frame.input_id = input.id;
  const auto output = value(stitched_output_generation_id(generation, 0.0, 32, 24));
  require(create_field_mask(
      directory.string(),
      [](const fs::path& image) {
        return cv::imwrite(image.string(), cv::Mat(24, 32, CV_8UC3, cv::Scalar(64, 64, 64)))
            ? absl::OkStatus()
            : absl::InternalError("calibration image");
      },
      output,
      {},
      {},
      {},
      description.frame));
  require(is_field_mask_configured_for_stitching_config(directory.string(), 0, 0.0), "initial timed mask is valid");
}
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  // GstDiscoverer autoplugs decoders while probing depth. Keep this regression
  // CPU-only even on hosts whose NVIDIA/VA decoders have the highest rank.
  GstElementFactory* software_decoder = gst_element_factory_find("avdec_h265");
  if (!software_decoder) {
    std::cerr << "FAIL: CPU H.265 decoder is required for depth probing\n";
    return 1;
  }
  gst_object_unref(software_decoder);
  GList* decoders = gst_element_factory_list_get_elements(
      GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO, GST_RANK_NONE);
  for (GList* item = decoders; item; item = item->next) {
    auto* feature = GST_PLUGIN_FEATURE(item->data);
    const std::string name = gst_plugin_feature_get_name(feature);
    gst_plugin_feature_set_rank(feature, name.rfind("avdec_", 0) == 0 ? GST_RANK_PRIMARY + 1 : GST_RANK_NONE);
  }
  gst_plugin_feature_list_free(decoders);
  char pattern[] = "/tmp/configurator-rink-precision-XXXXXX";
  const fs::path root = ::mkdtemp(pattern);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{root};
  try {
    const auto baseline = value(hm::baseline_config::load());
    ::setenv("HM_GAME_DIR", root.c_str(), 1);
    ::setenv("HM_TEST_RINK_FORCE_PROFILE", "1", 1);
    const fs::path eight = root / "8.mp4", ten = root / "10.mp4";
    // Tiny CPU-encoded streams exercise the production source-depth probe.
    for (bool high : {false, true}) {
      const std::string command =
          "ffmpeg -nostdin -loglevel error -f lavfi -i color=white:s=64x64:r=2 -frames:v 4 "
          "-threads 1 -c:v libx265 -x265-params pools=1:frame-threads=1:log-level=error -pix_fmt " +
          std::string(high ? "yuv420p10le " : "yuv420p ") + (high ? ten : eight).string();
      require(std::system(command.c_str()) == 0, "encode source depth fixture");
    }
    require(
        hm::getVideoBitDepth(eight.string()) == 8 && hm::getVideoBitDepth(ten.string()) == 10,
        "production probe identifies 8-bit and 10-bit media");
    struct Case {
      const char* name;
      bool source_ten;
      const char* stored;
      const char* mode;
      const char* ui;
      bool configure_only;
      bool resolved_ten;
      bool prepare;
    };
    const Case cases[] = {
        {"auto8-match", false, "rgba8", "auto", nullptr, false, false, false},
        {"auto10-match", true, "half4-bits", "auto", nullptr, false, true, false},
        {"auto10-prepare", true, "rgba8", "auto", nullptr, false, true, true},
        {"auto8-prepare", false, "half4-bits", "auto", nullptr, false, false, true},
        {"forced-on", false, "rgba8", "true", nullptr, false, true, true},
        {"forced-off", true, "half4-bits", "false", nullptr, false, false, true},
        {"ui-on", false, "rgba8", "false", "true", false, true, true},
        {"ui-off", true, "half4-bits", "true", "false", false, false, true},
        {"configure-only-change", true, "rgba8", "auto", nullptr, true, true, true},
        {"configure-only-match", false, "rgba8", "auto", nullptr, true, false, false},
    };
    for (const auto& test : cases) {
      fixture(
          root / test.name,
          baseline.values,
          test.source_ten ? ten : eight,
          test.stored,
          test.mode,
          test.configure_only);
      hm::Configurator configurator(test.name, baseline.root.string(), hm::Configurator::kUseConfigFileGpu);
      hm::ConfiguratorTestAccess::isolate_user_config(configurator);
      require(configurator.configure());
      if (test.ui)
        require(configurator.apply_config_item("hstream_ui.camera_controls.Use_10_Bit_Grading", test.ui));
      const auto status = configurator.complete_configuration(false);
      const bool expected_skip = test.configure_only && !test.prepare;
      require(
          expected_skip ? absl::IsCancelled(status) : status.ok(), test.name + std::string(": ") + status.ToString());
      require(
          configurator.config()["pipeline"]["hmstitcher"]["properties"]["high-bit-depth"].as<int>() ==
              (test.resolved_ten ? 1 : 0),
          test.name + std::string(": authoritative precision"));
      require(
          configurator.rink_mask_required() == test.prepare &&
              configurator.stitching_calibration_required() == test.prepare,
          test.name + std::string(": preparation flags"));
      if (test.prepare) {
        require(configurator.stitching_calibration_start_stage() == "rink-mask", "reuse solved alignment");
        const auto saved = YAML::LoadFile((root / test.name / "config.yaml").string());
        require(
            saved["hstream_ui"]["stitching_calibration"]["status"].as<std::string>() == "complete" &&
                saved["hstream_ui"]["stitching_calibration"]["rink_mask_status"].as<std::string>() == "pending" &&
                configurator.active_stitching_invalidation_id() == "precision-owner",
            "guarded completed-owner mask claim");
        if (test.configure_only)
          require(
              configurator.config()["pipeline"]["hmstitcher"]["configure-only"].as<int>() == 0 &&
                  configurator.config()["pipeline"]["hmstitcher"]["one-pass-mode"].as<int>() == 1,
              "configure-only mismatch reaches timed preparation");
      }
      std::cout << "PASS: " << test.name << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
