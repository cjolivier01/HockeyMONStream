#include "PlayerFrameScan.h"

#include <gstnvdsmeta.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <tuple>

#include "PlayerFrameDetectorIdentity.h"
#include "deepstream_app.h"
#include "hstream/src/gst-plugins/gst-fieldmask/fieldmask_payload.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/StitchingFramePairMeta.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

namespace hm::pipeline {
namespace {
namespace fs = std::filesystem;

absl::Status error(const std::string& message) {
  return absl::FailedPreconditionError("Player frame scan: " + message);
}

absl::StatusOr<NvDsFrameMeta*> single_frame(GstBuffer* buffer) {
  auto* batch = buffer ? gst_buffer_get_nvds_batch_meta(buffer) : nullptr;
  if (!batch || batch->num_frames_in_batch != 1 || !batch->frame_meta_list || batch->frame_meta_list->next)
    return error("expected exactly one stitched frame per inference batch");
  auto* frame = static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data);
  if (!frame || !GST_CLOCK_TIME_IS_VALID(frame->buf_pts))
    return error("missing stitched frame timestamp");
  return frame;
}

absl::StatusOr<std::string> detector_identity(const NvDsGieConfig& config) {
  if (!config.enable || !config.config_file_path || config.plugin_type != NV_DS_GIE_PLUGIN_INFER || config.interval)
    return error("requires the ordinary nvinfer primary detector with interval zero");
  return PlayerFrameDetectorModelIdentity(
      config.config_file_path,
      config.unique_id,
      config.model_engine_file_path ? fs::path(config.model_engine_file_path) : fs::path{});
}

absl::StatusOr<YAML::Node> game_config(const fs::path& directory) {
  auto config = stitching::load_game_config_file(directory / "config.yaml");
  if (!config.ok())
    return config.status();
  if (!config->has_value())
    return error("missing baseline config.yaml");
  return **config;
}

absl::StatusOr<std::string> generation(const fs::path& directory) {
  auto lock = stitching::HuginProject::RecoverAndLock(directory);
  if (!lock.ok())
    return lock.status();
  return stitching::HuginProject::GenerationId(directory, **lock);
}

std::string mask_settings(GstElement* element) {
  gfloat raise = 0, lower = 0;
  gboolean strict = FALSE;
  g_object_get(
      element,
      "raise-bbox-center-by-height-ratio",
      &raise,
      "lower-bbox-bottom-by-height-ratio",
      &lower,
      "require-existing-mask",
      &strict,
      nullptr);
  std::ostringstream result;
  result << std::setprecision(9) << "raise=" << raise << ";lower=" << lower << ";strict=" << strict;
  return result.str();
}

absl::StatusOr<stitching::PlayerFramePairIdentity> pair_identity(const hm::StitchingFramePair& pair) {
  stitching::PlayerFramePairIdentity result;
  result.timeline_pts_ns = pair.timeline_pts;
  const std::array<hm::DecodedFrameSequence, 2> cameras{pair.left, pair.right};
  for (size_t i = 0; i < 2; ++i) {
    const auto& camera = cameras[i];
    const char* uri = g_quark_to_string(camera.source_uri);
    if (!uri || !GST_CLOCK_TIME_IS_VALID(camera.source_pts))
      return error("missing exact source identity");
    gchar* hostname = nullptr;
    gchar* path = g_filename_from_uri(uri, &hostname, nullptr);
    const bool local = path && (!hostname || !*hostname || g_strcmp0(hostname, "localhost") == 0);
    if (local)
      result.cameras[i] = {path, camera.source_pts, camera.source_id, camera.sequence};
    g_free(path);
    g_free(hostname);
    if (!local)
      return error("selection requires local file-backed camera sources");
  }
  HM_RETURN_IF_ERROR(stitching::CanonicalizePlayerFramePair(&result));
  return result;
}
} // namespace

PlayerFrameScan::PlayerFrameScan(AppCtx* app, fs::path game_directory, stitching::PlayerFrameSelectionSettings settings)
    : app_(app),
      game_directory_(std::move(game_directory)),
      settings_(settings),
      timing_(settings.duration_ns, settings.interval_ns) {}

absl::StatusOr<std::unique_ptr<PlayerFrameScan>> PlayerFrameScan::Create(
    AppCtx* app,
    const fs::path& game_directory,
    const stitching::PlayerFrameSelectionSettings& settings,
    uint64_t decode_anchor_ns) {
  HM_RETURN_IF_ERROR(stitching::ValidatePlayerFrameSelectionSettings(settings));
  if (!app)
    return error("missing pipeline");
  auto result = std::unique_ptr<PlayerFrameScan>(new PlayerFrameScan(app, game_directory, settings));
  result->decode_anchor_ns_ = decode_anchor_ns;
  result->rotation_degrees_ = app->config.hmsticher_config.post_stitch_rotate_degrees;
  HM_ASSIGN_OR_RETURN(result->initial_generation_, generation(game_directory));
  HM_ASSIGN_OR_RETURN(
      result->expected_output_generation_, stitching::current_stitched_output_generation_id(game_directory));
  cv::Mat mask;
  HM_ASSIGN_OR_RETURN(mask, stitching::load_field_mask(game_directory, result->expected_output_generation_));
  HM_ASSIGN_OR_RETURN(result->expected_mask_fingerprint_, stitching::PlayerFrameMaskFingerprint(mask));
  HM_ASSIGN_OR_RETURN(result->detector_identity_, detector_identity(app->config.primary_gie_config));
  YAML::Node config;
  HM_ASSIGN_OR_RETURN(config, game_config(game_directory));
  HM_ASSIGN_OR_RETURN(
      result->context_["source_context"], stitching::player_frame_source_context(config, decode_anchor_ns));
  result->context_["decode_anchor_ns"] = std::to_string(decode_anchor_ns);
  result->context_["baseline_generation"] = result->initial_generation_;
  result->context_["output_generation"] = result->expected_output_generation_;
  result->context_["detector_identity"] = result->detector_identity_;
  result->context_["rink_mask_sha256"] = result->expected_mask_fingerprint_;
  result->context_["output_rotation_degrees"] = std::to_string(result->rotation_degrees_);
  return result;
}

PlayerFrameScan::~PlayerFrameScan() {
  for (const auto& [pad, id] : probes_) {
    gst_pad_remove_probe(pad, id);
    gst_object_unref(pad);
  }
}

absl::Status PlayerFrameScan::Attach() {
  auto* detector = app_->pipeline.common_elements.primary_gie_bin.bin;
  auto* fieldmask = app_->pipeline.dsfieldmask_bin.bin;
  auto* filter = app_->pipeline.dsfieldmask_bin.elem_dsfieldmask;
  auto* stitcher = app_->pipeline.hmstitcher_bin.elem_hmstitcher;
  if (!detector || !fieldmask || !filter || !stitcher)
    return error("analysis graph is missing inference, rink mask or stitching");
  guint width = 0;
  g_object_get(stitcher, "max-output-width", &width, nullptr);
  max_output_width_ = width;
  gboolean strict = FALSE;
  g_object_get(filter, "require-existing-mask", &strict, nullptr);
  if (!strict)
    return error("rink masking must require an existing mask");
  context_["fieldmask_settings"] = mask_settings(filter);
  for (const auto& [element, name, callback] :
       {std::tuple<GstElement*, const char*, GstPadProbeCallback>{detector, "sink", Gate},
        std::tuple<GstElement*, const char*, GstPadProbeCallback>{fieldmask, "src", Observe}}) {
    GstPad* pad = gst_element_get_static_pad(element, name);
    if (!pad)
      return error(std::string("missing analysis pad ") + name);
    const auto id = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, callback, this, nullptr);
    if (!id) {
      gst_object_unref(pad);
      return error("cannot attach analysis probe");
    }
    probes_.emplace_back(pad, id);
  }
  return absl::OkStatus();
}

GstPadProbeReturn PlayerFrameScan::Fail(GstPad*, const absl::Status& status) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (failure_.ok()) {
    failure_ = status;
    GError* cause = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, status.ToString().c_str());
    gst_element_post_message(
        app_->pipeline.pipeline,
        gst_message_new_error(GST_OBJECT(app_->pipeline.pipeline), cause, "Player frame scan failed"));
    g_error_free(cause);
  }
  return GST_PAD_PROBE_DROP;
}

GstPadProbeReturn PlayerFrameScan::Gate(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
  auto* self = static_cast<PlayerFrameScan*>(data);
  try {
    const auto selected = self->GateFrame(GST_PAD_PROBE_INFO_BUFFER(info));
    return selected.ok() ? (*selected ? GST_PAD_PROBE_OK : GST_PAD_PROBE_DROP) : self->Fail(pad, selected.status());
  } catch (const std::exception& exception) {
    return self->Fail(pad, error(exception.what()));
  } catch (...) {
    return self->Fail(pad, error("unknown sampling failure"));
  }
}

GstPadProbeReturn PlayerFrameScan::Observe(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
  auto* self = static_cast<PlayerFrameScan*>(data);
  try {
    const auto status = self->ObserveFrame(GST_PAD_PROBE_INFO_BUFFER(info));
    return status.ok() ? GST_PAD_PROBE_OK : self->Fail(pad, status);
  } catch (const std::exception& exception) {
    return self->Fail(pad, error(exception.what()));
  } catch (...) {
    return self->Fail(pad, error("unknown scoring failure"));
  }
}

absl::StatusOr<bool> PlayerFrameScan::GateFrame(GstBuffer* buffer) {
  NvDsFrameMeta* frame;
  HM_ASSIGN_OR_RETURN(frame, single_frame(buffer));
  const auto pair = hm::stitching_frame_pair(frame);
  if (!pair || !GST_CLOCK_TIME_IS_VALID(pair->timeline_pts))
    return error("stitcher did not preserve the synchronized raw camera pair");
  return timing_.Select(pair->timeline_pts);
}

absl::Status PlayerFrameScan::ObserveFrame(GstBuffer* buffer) {
  NvDsFrameMeta* frame;
  HM_ASSIGN_OR_RETURN(frame, single_frame(buffer));
  HM_RETURN_IF_ERROR(ValidatePlayerFrameScanMask(frame));
  const auto pair = hm::stitching_frame_pair(frame);
  if (!pair || !GST_CLOCK_TIME_IS_VALID(pair->timeline_pts))
    return error("sample lost its camera pair metadata");
  uint64_t elapsed_ns;
  HM_ASSIGN_OR_RETURN(elapsed_ns, timing_.Elapsed(pair->timeline_pts));
  const auto* output = stitching::find_stitched_output_generation_meta(frame);
  const auto* mask = hm::UserApplicationPayload::get_payload<hm::fieldmask::FieldMaskPayload>(frame);
  if (output->generation() != expected_output_generation_ || output->hugin_generation() != initial_generation_)
    return error("baseline geometry changed during the scan");
  if (elapsed_ns >= settings_.duration_ns)
    return timing_.Observe(pair->timeline_pts);
  if (observations_.size() >= settings_.maximum_observations)
    return error("observation limit exceeded");
  if (runtime_detector_identity_.empty()) {
    // Engine construction during preroll is expected. Bind the actual loaded
    // engine only after inference, then require these same bytes at shutdown.
    gchar* engine = nullptr;
    g_object_get(app_->pipeline.common_elements.primary_gie_bin.primary_gie, "model-engine-file", &engine, nullptr);
    if (engine)
      runtime_engine_path_ = engine;
    g_free(engine);
    HM_ASSIGN_OR_RETURN(
        runtime_detector_identity_,
        PlayerFrameDetectorRuntimeIdentity(app_->config.primary_gie_config.config_file_path, runtime_engine_path_));
    context_["detector_model_identity"] = detector_identity_;
    context_["detector_runtime_identity"] = runtime_detector_identity_;
    const std::string material = detector_identity_ + ":" + runtime_detector_identity_;
    gchar* combined = g_compute_checksum_for_string(G_CHECKSUM_SHA256, material.c_str(), material.size());
    context_["detector_identity"] = combined;
    g_free(combined);
  }
  if (context_["rink_mask_revision"].empty()) {
    std::string fingerprint;
    HM_ASSIGN_OR_RETURN(fingerprint, stitching::PlayerFrameMaskFingerprint(mask->mask()));
    if (fingerprint != expected_mask_fingerprint_)
      return error("loaded mask differs from the baseline");
    context_["rink_mask_revision"] = mask->revision();
  } else if (context_["rink_mask_revision"] != mask->revision()) {
    return error("rink-mask authority changed during the scan");
  }
  if (!overlap_) {
    HM_ASSIGN_OR_RETURN(
        overlap_,
        stitching::LoadPlayerFrameOverlap(
            game_directory_,
            {cv::Size(pair->left_width, pair->left_height), cv::Size(pair->right_width, pair->right_height)},
            max_output_width_,
            rotation_degrees_));
    if (overlap_->generation_id != initial_generation_)
      return error("overlap maps belong to a different baseline generation");
  }
  if (overlap_->canvas_size != cv::Size(frame->source_frame_width, frame->source_frame_height) ||
      !frame->pipeline_width || !frame->pipeline_height)
    return error("analysis geometry differs from overlap maps");
  stitching::PlayerFramePairIdentity identity;
  HM_ASSIGN_OR_RETURN(identity, pair_identity(*pair));
  for (const auto& camera : identity.cameras) {
    if (std::none_of(
            sources_.begin(), sources_.end(), [&](const auto& source) { return source.path == camera.path; })) {
      stitching::PlayerFrameSourceBinding binding;
      HM_ASSIGN_OR_RETURN(binding, stitching::BindPlayerFrameSource(camera.path));
      sources_.push_back(std::move(binding));
    }
  }
  std::vector<stitching::PlayerFrameBox> boxes;
  const double sx = static_cast<double>(frame->source_frame_width) / frame->pipeline_width;
  const double sy = static_cast<double>(frame->source_frame_height) / frame->pipeline_height;
  for (auto* item = frame->obj_meta_list; item; item = item->next) {
    const auto* object = static_cast<NvDsObjectMeta*>(item->data);
    if (!object || object->unique_component_id != static_cast<int>(app_->config.primary_gie_config.unique_id) ||
        object->class_id != 0)
      continue;
    auto box = object->detector_bbox_info.org_bbox_coords;
    if (box.width <= 0 || box.height <= 0)
      box = {object->rect_params.left, object->rect_params.top, object->rect_params.width, object->rect_params.height};
    boxes.push_back({box.left * sx, box.top * sy, box.width * sx, box.height * sy, object->confidence, 0});
    if (boxes.size() > 256)
      return error("too many rink-filtered people in a sampled frame");
  }
  stitching::PlayerFrameObservation observation;
  HM_ASSIGN_OR_RETURN(observation, stitching::ScorePlayerFrame(identity, boxes, overlap_->mask, overlap_->canvas_size));
  observations_.push_back(std::move(observation));
  return timing_.Observe(pair->timeline_pts);
}

absl::Status PlayerFrameScan::Finish(const fs::path& report_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  HM_RETURN_IF_ERROR(failure_);
  std::string current;
  HM_ASSIGN_OR_RETURN(current, generation(game_directory_));
  if (current != initial_generation_)
    return error("baseline generation changed before scan completion");
  HM_ASSIGN_OR_RETURN(current, stitching::current_stitched_output_generation_id(game_directory_));
  if (current != expected_output_generation_)
    return error("output geometry changed before scan completion");
  cv::Mat mask;
  HM_ASSIGN_OR_RETURN(mask, stitching::load_field_mask(game_directory_, expected_output_generation_));
  HM_ASSIGN_OR_RETURN(current, stitching::PlayerFrameMaskFingerprint(mask));
  if (current != expected_mask_fingerprint_)
    return error("rink mask changed before scan completion");
  HM_ASSIGN_OR_RETURN(current, detector_identity(app_->config.primary_gie_config));
  if (current != detector_identity_)
    return error("detector inputs changed before scan completion");
  if (!runtime_detector_identity_.empty()) {
    HM_ASSIGN_OR_RETURN(
        current,
        PlayerFrameDetectorRuntimeIdentity(app_->config.primary_gie_config.config_file_path, runtime_engine_path_));
    if (current != runtime_detector_identity_)
      return error("loaded detector engine/parser changed before scan completion");
  }
  YAML::Node config;
  HM_ASSIGN_OR_RETURN(config, game_config(game_directory_));
  HM_ASSIGN_OR_RETURN(current, stitching::player_frame_source_context(config, decode_anchor_ns_));
  if (current != context_["source_context"])
    return error("source settings changed before scan completion");
  for (const auto& source : sources_) {
    stitching::PlayerFrameSourceBinding current_binding;
    HM_ASSIGN_OR_RETURN(current_binding, stitching::BindPlayerFrameSource(source.path));
    if (current_binding.path != source.path || current_binding.size != source.size ||
        current_binding.modification_time_ns != source.modification_time_ns)
      return error("source file changed before scan completion");
  }
  stitching::PlayerFrameSelectionReport report;
  if (observations_.empty()) {
    report.unavailable_reason = "No synchronized inferred frames were observed before EOS";
  } else {
    HM_ASSIGN_OR_RETURN(report, stitching::SelectPlayerFrames(settings_, observations_, sources_, context_));
  }
  const fs::path temporary = report_path.string() + ".tmp-" + std::to_string(getpid());
  std::error_code ec;
  {
    std::ofstream output(temporary, std::ios::trunc);
    output << stitching::PlayerFrameSelectionReportYaml(report) << '\n';
    output.close();
    if (!output) {
      fs::remove(temporary, ec);
      return error("cannot write selection report");
    }
  }
  fs::rename(temporary, report_path, ec);
  if (ec) {
    fs::remove(temporary);
    return error("cannot publish selection report: " + ec.message());
  }
  g_print(
      "HSTREAM_PLAYER_FRAME_SCAN observations=%zu selected=%zu report=%s\n",
      report.observation_count,
      report.plan ? report.plan->selected.size() : 0,
      report_path.c_str());
  return absl::OkStatus();
}
} // namespace hm::pipeline
