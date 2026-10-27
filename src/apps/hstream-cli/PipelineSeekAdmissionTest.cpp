#include "hstream/src/libs/common/Status.h"
#include "src/apps/hstream-cli/PipelineApp.h"

#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string output;
void Capture(const gchar* text) {
  output += text;
}
} // namespace

struct PipelineSeekAdmissionTestAccess {
  PipelineApplication application;
  std::shared_ptr<HmApp> context = std::make_shared<HmApp>("seek-admission", "", 0);

  PipelineSeekAdmissionTestAccess() {
    application.stage_app_contexts_[0] = {context};
    application.enabled_sink_types_ = {{NV_DS_SINK_WEBRTC}};
    application.stitching_calibration_only_ = true;
    application.web_preview_run_id_ = "01234567-89ab-cdef-0123-456789abcdef";
    context->config.num_source_sub_bins = 1;
    context->config.multi_source_config[0].enable = TRUE;
    context->config.multi_source_config[0].type = NV_DS_SOURCE_URI_MULTIPLE;
    context->config.num_sink_sub_bins = 1;
    context->config.sink_bin_sub_bin_config[0].enable = TRUE;
    context->config.sink_bin_sub_bin_config[0].type = NV_DS_SINK_WEBRTC;
    const auto options = hm::preview::ReadWebPreviewOptions(YAML::LoadFile("configs/baseline.yaml"));
    Check(options.ok(), "Invalid baseline preview options");
    // Admission needs an owned session, not an encoder, socket or GPU graph.
    application.web_preview_.reset(new hm::preview::WebPreviewSession(*context, *options));
    application.inspector_topology_generation_ = 1;
    application.web_preview_->routes_.bind_session({application.web_preview_run_id_, 0, 1});
  }

  bool Admitted() const {
    return application.runtime_seek_is_local_render_only();
  }

  void Run() {
    Check(Admitted(), "Completed Calibration browser playback cannot seek");
    output.clear();
    application.publish_web_preview_capability();
    Check(
        output.find("HSTREAM_PREVIEW_PRESENTATION ") != std::string::npos &&
            output.find("\"seek\":true") != std::string::npos,
        "Completed Calibration did not advertise seek support");
    application.stitching_calibration_only_ = false;
    Check(Admitted(), "Program browser playback cannot seek");
    application.stitching_calibration_only_ = true;
    context->config.multi_source_config[0].type = NV_DS_SOURCE_URI;
    Check(Admitted(), "Single-file Calibration playback cannot seek");
    context->config.multi_source_config[0].type = NV_DS_SOURCE_CAMERA_V4L2;
    Check(!Admitted(), "Live source admitted a browser seek");
    context->config.multi_source_config[0].type = NV_DS_SOURCE_URI_MULTIPLE;
    context->config.multi_source_config[0].enable = FALSE;
    Check(!Admitted(), "Missing file source admitted a browser seek");
    context->config.multi_source_config[0].enable = TRUE;

    for (const auto type :
         {NV_DS_SINK_ENCODE_FILE,
          NV_DS_SINK_ENCODE_STITCHED_FILE,
          NV_DS_SINK_ENCODE_PROGRAM_4K_FILE,
          NV_DS_SINK_UDPSINK}) {
      application.enabled_sink_types_[0].insert(type);
      Check(!Admitted(), "Requested archive/network sink admitted a browser seek");
      application.enabled_sink_types_[0].erase(type);
      context->config.num_sink_sub_bins = 2;
      context->config.sink_bin_sub_bin_config[1].enable = TRUE;
      context->config.sink_bin_sub_bin_config[1].type = type;
      Check(!Admitted(), "Active archive/network sink admitted a browser seek");
      context->config.num_sink_sub_bins = 1;
    }
    context->config.sink_bin_sub_bin_config[0].enable = FALSE;
    Check(!Admitted(), "No active preview sink admitted a browser seek");
    context->config.sink_bin_sub_bin_config[0].enable = TRUE;
    context->config.dsplaytracker_config.enable = TRUE;
    for (const auto name : {"telemetry-csv-dir", "telemetry-db-dir", "telemetry_csv_dir", "telemetry_db_dir"}) {
      context->config.dsplaytracker_config.private_properties = {{name, "/unused/test-telemetry"}};
      Check(!Admitted(), "Active telemetry admitted a browser seek");
    }
    context->config.dsplaytracker_config.private_properties.clear();
    context->config.dsplaytracker_config.enable = FALSE;
    application.stage_app_contexts_[0].push_back(context);
    Check(!Admitted(), "Multiple pipelines admitted a browser seek");
    application.stage_app_contexts_[0].pop_back();
    application.web_preview_run_id_.clear();
    Check(!Admitted(), "Unowned WebRTC sink admitted a browser seek");
    application.web_preview_run_id_ = "01234567-89ab-cdef-0123-456789abcdef";
    Check(Admitted(), "Calibration admission did not recover after rejected states");

    application.stitch_frame_calibration_active_.store(true);
    output.clear();
    Check(!application.seek_runtime(1000000000, 7), "Seek during active calibration succeeded");
    Check(
        output.find("status=rejected generation=7 reason=stitching-calibration-active") != std::string::npos,
        "Active calibration was not rejected before graph mutation");
    Check(
        !application.runtime_seek_pending_ && !application.runtime_seek_recreation_active_.load(),
        "Rejected seek scheduled graph reconstruction");
  }
};

int main() {
  gst_init(nullptr, nullptr);
  g_set_print_handler(Capture);
  try {
    PipelineSeekAdmissionTestAccess fixture;
    fixture.Run();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
