#include "hstream/src/gst-plugins/gst-videoprep/stitcher/stitcher.h"

#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/preputils.h"
#include "hstream/src/libs/common/DecodedFrameSequenceMeta.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "cupano/cuda/cudaStatus.h"
#include "cupano/pano/cudaMat.h"

// #include "deepstream/sources/includes/nvbufsurface.h"
#include "nvbufsurface.h"

#include <cuda_runtime.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gstreamer-1.0/gst/gstinfo.h>
#include <npp.h>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "nvdsmeta.h"

#include <assert.h>
#include <cuda.h>
#include <unistd.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(__aarch64__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "cudaEGL.h"
#endif

#include <algorithm>

namespace hm {
namespace stitcher {

namespace {

OnePassCalibrationCompletionLatch process_calibration_completion_latch;

std::string calibration_message(std::string message) {
  std::replace(message.begin(), message.end(), '\n', ' ');
  std::replace(message.begin(), message.end(), '\r', ' ');
  return message;
}

void report_calibration_progress(const char* stage, const char* status, const std::string& message = {}) {
  const std::string single_line_message = calibration_message(message);
  g_print(
      "HSTREAM_CALIBRATION stage=%s status=%s%s%s\n",
      stage,
      status,
      single_line_message.empty() ? "" : " message=",
      single_line_message.c_str());
  std::fflush(stdout);
}

absl::Status report_calibration_failure(const absl::Status& status) {
  report_calibration_progress("calibration", "failed", status.ToString());
  return status;
}

bool calibration_progress_requested() {
  const char* value = g_getenv("HSTREAM_CALIBRATION_PENDING");
  return value && value[0] != '\0' && g_strcmp0(value, "0") != 0;
}

bool calibration_starts_from_control_points() {
  return g_strcmp0(g_getenv("HSTREAM_CALIBRATION_START_STAGE"), "features") == 0;
}

std::string calibration_completion_scope(
    const std::string& output_generation,
    const std::string& invalidation_id,
    const std::string& run_generation) {
  std::ostringstream scope;
  scope << output_generation.size() << ':' << output_generation << invalidation_id.size() << ':' << invalidation_id
        << run_generation.size() << ':' << run_generation;
  return scope.str();
}

void log_canvas_hint(const std::string& prefix, size_t width, size_t height) {
  if (!width || !height) {
    return;
  }
  g_print("%s canvas hint: %zu x %zu\n", prefix.c_str(), width, height);
}

bool log_batches_enabled() {
  static const bool enabled = [] {
    const char* value = g_getenv("HMSTITCHER_LOG_BATCHES");
    return value && value[0] != '\0' && g_strcmp0(value, "0") != 0;
  }();
  return enabled;
}

bool parse_finite_double(const std::string& value, double& parsed) {
  if (value.empty()) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  parsed = std::strtod(value.c_str(), &end);
  if (value.c_str() == end || errno == ERANGE || !std::isfinite(parsed)) {
    return false;
  }
  while (end && *end && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  return end && *end == '\0';
}

std::string normalized_property_value(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    if (c == '_') {
      return '-';
    }
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

} // namespace

static constexpr int kNumStitcherLaplacianLevels = 11;

OnePassCalibrationProgressPlan one_pass_calibration_progress_plan(
    bool configured_during_run,
    bool mask_configured,
    bool report_latched,
    bool process_completion_latched) {
  const bool create_mask = !mask_configured;
  const bool report = report_latched ||
      (!process_completion_latched && (configured_during_run || calibration_progress_requested() || create_mask));
  return {
      .report = report,
      .create_mask = create_mask,
      .complete = report && mask_configured,
  };
}

bool OnePassCalibrationCompletionLatch::delivered(const std::string& calibration_scope) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto state = state_by_scope_.find(calibration_scope);
  return state != state_by_scope_.end() && state->second == 2;
}

bool OnePassCalibrationCompletionLatch::try_begin_delivery(const std::string& calibration_scope) {
  std::lock_guard<std::mutex> lock(mutex_);
  unsigned& state = state_by_scope_[calibration_scope];
  if (state != 0)
    return false;
  state = 1;
  return true;
}

void OnePassCalibrationCompletionLatch::finish_delivery(const std::string& calibration_scope, bool delivered) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (delivered) {
    state_by_scope_[calibration_scope] = 2;
  } else {
    state_by_scope_.erase(calibration_scope);
  }
}

StitcherPriv::~StitcherPriv() {
  Shutdown();
}

void StitcherPriv::Shutdown() {
  Super::Shutdown();
  {
    std::lock_guard<std::mutex> lock(eos_mu_);
    eos_snapshot_by_surface_.clear();
  }
  {
    absl::MutexLock lk(&stitcher_mu_);
    stitcher_fp32_.reset();
    stitcher_fp16_.reset();
    hugin_generation_id_.clear();
  }
  calibration_invalidation_id_.clear();
  calibration_run_generation_.clear();
  release_rotation_scratch();
}

bool StitcherPriv::HandleEvent(GstEvent* event) {
  if (!event) {
    return true;
  }
  if ((GstNvEventType)GST_EVENT_TYPE(event) == GST_NVEVENT_STREAM_EOS) {
    guint source_id = 0;
    gst_nvevent_parse_stream_eos(event, &source_id);
    {
      std::lock_guard<std::mutex> lock(eos_mu_);
      eos_source_ids_.insert(source_id);
    }
    return Super::HandleEvent(event);
  }
  if ((GstNvEventType)GST_EVENT_TYPE(event) == GST_NVEVENT_STREAM_START) {
    guint source_id = 0;
    gchar* stream_id = nullptr;
    gst_nvevent_parse_stream_start(event, &source_id, &stream_id);
    {
      std::lock_guard<std::mutex> lock(eos_mu_);
      eos_source_ids_.erase(source_id);
      eos_snapshot_by_surface_.clear();
      pipeline_eos_seen_ = false;
    }
    return Super::HandleEvent(event);
  }
  if ((GstNvEventType)GST_EVENT_TYPE(event) == GST_NVEVENT_STREAM_RESET) {
    guint source_id = 0;
    gst_nvevent_parse_stream_reset(event, &source_id);
    {
      std::lock_guard<std::mutex> lock(eos_mu_);
      eos_source_ids_.erase(source_id);
      eos_snapshot_by_surface_.clear();
    }
    return Super::HandleEvent(event);
  }
  if ((GstNvEventType)GST_EVENT_TYPE(event) == GST_NVEVENT_PAD_ADDED) {
    guint source_id = 0;
    gst_nvevent_parse_pad_added(event, &source_id);
    {
      std::lock_guard<std::mutex> lock(eos_mu_);
      eos_source_ids_.erase(source_id);
      eos_snapshot_by_surface_.clear();
      pipeline_eos_seen_ = false;
    }
    return Super::HandleEvent(event);
  }
  if (GST_EVENT_TYPE(event) == GST_EVENT_STREAM_START) {
    {
      std::lock_guard<std::mutex> lock(eos_mu_);
      eos_source_ids_.clear();
      eos_snapshot_by_surface_.clear();
      pipeline_eos_seen_ = false;
    }
    return Super::HandleEvent(event);
  }
  if (GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
    {
      std::lock_guard<std::mutex> lock(eos_mu_);
      pipeline_eos_seen_ = true;
    }
    const bool handled = Super::HandleEvent(event);
    return handled;
  }
  return Super::HandleEvent(event);
}

StitcherPriv::EosSnapshot StitcherPriv::snapshot_eos_for_buffer(GstBuffer* inbuf) {
  EosSnapshot snapshot;
  GstMapInfo in_map_info = GST_MAP_INFO_INIT;
  if (!inbuf || !gst_buffer_map(inbuf, &in_map_info, GST_MAP_READ)) {
    std::lock_guard<std::mutex> lock(eos_mu_);
    snapshot.pipeline_eos_seen = pipeline_eos_seen_;
    snapshot.source_ids = eos_source_ids_;
    return snapshot;
  }

  auto* in_surface = reinterpret_cast<NvBufSurface*>(in_map_info.data);
  {
    std::lock_guard<std::mutex> lock(eos_mu_);
    snapshot.pipeline_eos_seen = pipeline_eos_seen_;
    snapshot.source_ids = eos_source_ids_;
    if (in_surface) {
      eos_snapshot_by_surface_[in_surface] = snapshot;
    }
  }
  gst_buffer_unmap(inbuf, &in_map_info);
  return snapshot;
}

StitcherPriv::EosSnapshot StitcherPriv::snapshot_eos_for_surface(NvBufSurface* in_surface) {
  std::lock_guard<std::mutex> lock(eos_mu_);
  auto snapshot = EosSnapshot{
      .pipeline_eos_seen = pipeline_eos_seen_,
      .source_ids = eos_source_ids_,
  };
  auto iter = eos_snapshot_by_surface_.find(in_surface);
  if (iter != eos_snapshot_by_surface_.end()) {
    // Sink-pad buffers and serialized EOS/stream events have a defined order. Preserve the state from the moment this
    // buffer was enqueued: retroactively merging a later global EOS can turn an earlier partial into a terminal
    // cancellation, causing OutputThread to discard other valid buffers that were already queued ahead of EOS.
    snapshot = iter->second;
    eos_snapshot_by_surface_.erase(iter);
  }
  return snapshot;
}

BufferResult StitcherPriv::ProcessBuffer(GstBuffer* inbuf) {
  snapshot_eos_for_buffer(inbuf);
  const BufferResult result = Super::ProcessBuffer(inbuf);
  if (result != BufferResult::Buffer_Async) {
    GstMapInfo in_map_info = GST_MAP_INFO_INIT;
    if (inbuf && gst_buffer_map(inbuf, &in_map_info, GST_MAP_READ)) {
      auto* in_surface = reinterpret_cast<NvBufSurface*>(in_map_info.data);
      {
        std::lock_guard<std::mutex> lock(eos_mu_);
        eos_snapshot_by_surface_.erase(in_surface);
      }
      gst_buffer_unmap(inbuf, &in_map_info);
    }
  }
  return result;
}

namespace {

std::string format_source_ids(const std::set<guint>& source_ids) {
  std::stringstream out;
  out << "{";
  bool first = true;
  for (guint source_id : source_ids) {
    if (!first) {
      out << ",";
    }
    out << source_id;
    first = false;
  }
  out << "}";
  return out.str();
}

absl::Status frame_sequence_mismatch_status(
    const std::string& reason,
    size_t frame_groups,
    size_t num_filled,
    size_t frame_meta_count,
    size_t duplicate_frame_sources,
    size_t incomplete_frame_groups,
    size_t inconsistent_source_groups,
    size_t invalid_surfaces_per_frame,
    const std::set<guint>& observed_source_ids,
    const std::set<guint>& missing_eos_source_ids,
    const std::set<guint>& eos_source_ids,
    bool pipeline_eos_explains_mismatch,
    bool pipeline_eos_seen) {
  std::stringstream message;
  message << "Stitcher did not receive the expected source/frame sequence"
          << " (reason=" << reason << ", frame_groups=" << frame_groups << ", num_filled=" << num_filled
          << ", frame_meta_count=" << frame_meta_count << ", duplicate_frame_sources=" << duplicate_frame_sources
          << ", incomplete_frame_groups=" << incomplete_frame_groups
          << ", inconsistent_source_groups=" << inconsistent_source_groups
          << ", invalid_surfaces_per_frame=" << invalid_surfaces_per_frame
          << ", observed_sources=" << format_source_ids(observed_source_ids)
          << ", source_eos_seen=" << eos_source_ids.size()
          << ", missing_eos_sources=" << format_source_ids(missing_eos_source_ids)
          << ", pipeline_eos_seen=" << pipeline_eos_seen << ")";

  if (pipeline_eos_explains_mismatch) {
    return absl::CancelledError(message.str());
  }
  return absl::FailedPreconditionError(message.str());
}

} // namespace

absl::StatusOr<std::pair<size_t, size_t>> select_runtime_stitch_pair(
    const std::vector<RuntimeFrameKey>& frames,
    const std::set<guint>& eos_source_ids,
    bool pipeline_eos_seen) {
  if (frames.empty()) {
    return absl::FailedPreconditionError("Runtime stitching requires at least one input frame");
  }

  std::map<guint, std::vector<size_t>> source_indices;
  std::map<gint, std::map<guint, size_t>> frame_indices;
  std::set<std::pair<gint, guint>> unique_frame_keys;
  for (size_t index = 0; index < frames.size(); ++index) {
    if (!unique_frame_keys.emplace(frames[index].frame_num, frames[index].source_id).second) {
      return absl::FailedPreconditionError("Runtime stitching received a duplicate frame/source pair");
    }
    source_indices[frames[index].source_id].push_back(index);
    frame_indices[frames[index].frame_num].emplace(frames[index].source_id, index);
  }
  if (source_indices.size() > 2) {
    return absl::FailedPreconditionError("Runtime stitching received frames from more than two sources");
  }

  if (source_indices.size() == 2 && frames.size() % 2 == 0 && frame_indices.size() * 2 == frames.size()) {
    const std::set<guint> expected_source_ids{source_indices.begin()->first, std::next(source_indices.begin())->first};
    bool every_frame_has_both_sources = true;
    for (const auto& [frame_num, by_source] : frame_indices) {
      (void)frame_num;
      std::set<guint> source_ids;
      for (const auto& [source_id, index] : by_source) {
        (void)index;
        source_ids.insert(source_id);
      }
      every_frame_has_both_sources = every_frame_has_both_sources && source_ids == expected_source_ids;
    }
    if (every_frame_has_both_sources) {
      std::vector<gint> complete_frame_numbers;
      complete_frame_numbers.reserve(frame_indices.size());
      for (const auto& [frame_num, by_source] : frame_indices) {
        (void)by_source;
        complete_frame_numbers.push_back(frame_num);
      }
      const auto continuity = validate_stitch_frame_continuity(complete_frame_numbers);
      if (!continuity.ok()) {
        return continuity.status();
      }
      const auto& first_pair = frame_indices.begin()->second;
      return std::make_pair(first_pair.begin()->second, first_pair.rbegin()->second);
    }
  }

  std::set<guint> observed_source_ids;
  for (const auto& [source_id, indices] : source_indices) {
    (void)indices;
    observed_source_ids.insert(source_id);
  }
  std::set<guint> observed_or_eos_source_ids = observed_source_ids;
  observed_or_eos_source_ids.insert(eos_source_ids.begin(), eos_source_ids.end());
  const bool source_eos_explains_unpairable_batch = !eos_source_ids.empty() && observed_or_eos_source_ids.size() <= 2;
  const bool structurally_valid_partial = frames.size() % 2 != 0 &&
      ((source_indices.size() == 1 && frames.size() == 1) ||
       (source_indices.size() == 2 &&
        std::abs(
            static_cast<std::ptrdiff_t>(source_indices.begin()->second.size()) -
            static_cast<std::ptrdiff_t>(std::next(source_indices.begin())->second.size())) == 1));
  if (pipeline_eos_seen || source_eos_explains_unpairable_batch) {
    return absl::CancelledError("Runtime stitching input ended before a complete source pair arrived");
  }

  if (structurally_valid_partial) {
    return absl::FailedPreconditionError(
        "nvstreammux emitted an incomplete batch before either camera permanently ended");
  }

  return absl::FailedPreconditionError("Could not find a balanced left/right frame pair for runtime stitching");
}

absl::Status validate_decoder_mux_sequence(const NvDsFrameMeta* frame_meta, bool* decoder_sequence_present) {
  const std::optional<DecodedFrameSequence> decoded_sequence = hm::decoded_frame_sequence(frame_meta);
  *decoder_sequence_present = decoded_sequence.has_value();
  if (!decoded_sequence.has_value()) {
    return absl::OkStatus();
  }
  if (decoded_sequence->source_id != frame_meta->source_id ||
      decoded_sequence->sequence != static_cast<uint64_t>(frame_meta->frame_num)) {
    return absl::FailedPreconditionError(
        absl::StrFormat(
            "Lossless stitching decode/mux invariant failed: source %u decoder frame %llu became mux source %u frame %d",
            decoded_sequence->source_id,
            static_cast<unsigned long long>(decoded_sequence->sequence),
            frame_meta->source_id,
            frame_meta->frame_num));
  }
  return absl::OkStatus();
}

absl::StatusOr<gint> validate_stitch_frame_continuity(
    const std::vector<gint>& frame_numbers,
    std::optional<gint> previous_frame_num) {
  if (frame_numbers.empty()) {
    return absl::FailedPreconditionError("Stitching received no complete frame numbers");
  }

  gint expected = 0;
  if (previous_frame_num.has_value()) {
    if (*previous_frame_num == std::numeric_limits<gint>::max()) {
      return absl::FailedPreconditionError("Stitching frame counter overflowed");
    }
    expected = *previous_frame_num + 1;
  }

  for (size_t index = 0; index < frame_numbers.size(); ++index) {
    const gint frame_num = frame_numbers[index];
    if (frame_num != expected) {
      return absl::FailedPreconditionError(
          absl::StrFormat(
              "Lossless stitching frame invariant failed: expected both cameras at frame %d, received frame %d",
              expected,
              frame_num));
    }
    if (index + 1 < frame_numbers.size()) {
      if (expected == std::numeric_limits<gint>::max()) {
        return absl::FailedPreconditionError("Stitching frame counter overflowed");
      }
      ++expected;
    }
  }
  return frame_numbers.back();
}

absl::Status prepare_stitch_output_surface(NvBufSurface* output_surface, size_t planned_frames) {
  if (!output_surface || !output_surface->batchSize) {
    return absl::FailedPreconditionError("Stitching output surface has no allocation capacity");
  }
  if (planned_frames > output_surface->batchSize) {
    return absl::FailedPreconditionError(
        absl::StrFormat(
            "Stitching planned %zu output frames but the surface capacity is %u",
            planned_frames,
            output_surface->batchSize));
  }
  output_surface->numFilled = 0;
  return absl::OkStatus();
}

absl::Status StitcherPriv::ensure_stitcher() {
  if (configure_only_ && !one_pass_mode_) {
    return absl::OkStatus();
  }
  if (config_file_.empty()) {
    return absl::NotFoundError("No control masks to load");
  }

  // In one-pass mode, defer loading until configuration has produced the mapping artifacts. Once mappings exist, every
  // mode validates the seam before hm-cupano loads it: enblend may save a cropped PNG with an oFFs origin that
  // hm-cupano does not interpret itself.
  if (one_pass_mode_) {
    auto is_configured = hm::stitching::is_stitching_configured(config_file_);
    if (!is_configured.ok()) {
      return is_configured.status();
    }
    if (!is_configured.value()) {
      if (!logged_missing_masks_) {
        g_print("hmstitcher: control masks in %s are missing or need regeneration\n", config_file_.c_str());
        logged_missing_masks_ = true;
      }
      return absl::OkStatus();
    }
  }

  const absl::Status seam_status = hm::stitching::maybe_create_default_seam_file(config_file_);
  if (!seam_status.ok())
    return seam_status;

  auto artifact_lock = hm::stitching::HuginProject::RecoverAndLock(config_file_);
  if (!artifact_lock.ok()) {
    return artifact_lock.status();
  }
  absl::MutexLock lk(&stitcher_mu_);
  if (!has_stitcher()) {
    hm::pano::ControlMasks control_masks;
    if (!control_masks.load(config_file_)) {
      std::string config_file_dir = config_file_;
      if (one_pass_mode_) {
        // In one-pass mode, allow the pipeline to bootstrap without masks.
        if (!logged_missing_masks_) {
          g_print("hmstitcher: missing control masks in %s\n", config_file_dir.c_str());
          logged_missing_masks_ = true;
        }
        return absl::OkStatus();
      } else {
        // Don't try again unless one-pass mode wants to re-attempt after configure.
        config_file_.clear();
        return absl::NotFoundError(TO_STRING("Could not load control masks from " << config_file_dir));
      }
    }
    auto generation = hm::stitching::HuginProject::GenerationId(config_file_, **artifact_lock);
    if (!generation.ok()) {
      return generation.status();
    }
    hugin_generation_id_ = std::move(*generation);
    update_canvas_hints(control_masks.canvas_width(), control_masks.canvas_height());
    if (stitch_compute_precision_ == StitchComputePrecision::kFp16) {
      g_print("hmstitcher: using fp16 stitch compute\n");
      stitcher_fp16_ = std::make_unique<STITCHER_FP16>(
          /*batch_size=*/1,
          /*num_levels=*/kNumStitcherLaplacianLevels,
          control_masks,
          /*match_exposure=*/match_exposure_,
          /*quiet=*/false,
          /*minimize_blend=*/minimize_blend_);
    } else {
      g_print("hmstitcher: using fp32 stitch compute\n");
      stitcher_fp32_ = std::make_unique<STITCHER_FP32>(
          /*batch_size=*/1,
          /*num_levels=*/kNumStitcherLaplacianLevels,
          control_masks,
          /*match_exposure=*/match_exposure_,
          /*quiet=*/false,
          /*minimize_blend=*/minimize_blend_);
    }
  }
  if (stitcher_fp16_ && !stitcher_fp16_->status().ok()) {
    return to_status(stitcher_fp16_->status());
  }
  if (stitcher_fp32_ && !stitcher_fp32_->status().ok()) {
    return to_status(stitcher_fp32_->status());
  }
  return absl::OkStatus();
}

absl::Status StitcherPriv::reload_stitcher() {
  HM_RETURN_IF_ERROR(ensure_stitcher());
  absl::MutexLock lk(&stitcher_mu_);
  if (stitcher_fp16_) {
    update_canvas_hints(stitcher_fp16_->canvas_width(), stitcher_fp16_->canvas_height());
    log_canvas_hint("hmstitcher", canvas_width_hint_, canvas_height_hint_);
  } else if (stitcher_fp32_) {
    update_canvas_hints(stitcher_fp32_->canvas_width(), stitcher_fp32_->canvas_height());
    log_canvas_hint("hmstitcher", canvas_width_hint_, canvas_height_hint_);
  }
  return absl::OkStatus();
}

absl::Status StitcherPriv::PreCapsInit(DSCustom_CreateParams* params) {
  owner_element_ = params && params->m_element ? GST_ELEMENT(params->m_element) : nullptr;
  if (params->config_file) {
    config_file_ = params->config_file;
  }
  const char* calibration_invalidation_id = g_getenv("HSTREAM_CALIBRATION_INVALIDATION_ID");
  calibration_invalidation_id_ = calibration_invalidation_id ? calibration_invalidation_id : "";
  absl::Status status = ensure_stitcher();
  if (!status.ok()) {
    if (one_pass_mode_ && absl::IsNotFound(status)) {
      if (!logged_missing_masks_) {
        g_print(
            "hmstitcher: control masks not found in %s; enabling one-pass configure on first batch\n",
            config_file_.c_str());
        logged_missing_masks_ = true;
      }
    } else {
      std::cerr << status << std::endl;
      return status;
    }
  }

  // Not an in-place transform
  m_transformMode = true;

  m_inVideoFmt = GST_VIDEO_FORMAT_RGBA;
  m_outVideoFmt = GST_VIDEO_FORMAT_RGBA;

  {
    absl::MutexLock lk(&stitcher_mu_);
    if (stitcher_fp16_) {
      // TODO: handle this through caps
      params->output_width_height[0] = stitcher_fp16_->canvas_width();
      params->output_width_height[1] = stitcher_fp16_->canvas_height();
      g_print(
          "Stitched canvas size: %d x %d\n", (int)stitcher_fp16_->canvas_width(), (int)stitcher_fp16_->canvas_height());
      update_canvas_hints(stitcher_fp16_->canvas_width(), stitcher_fp16_->canvas_height());
    } else if (stitcher_fp32_) {
      // TODO: handle this through caps
      params->output_width_height[0] = stitcher_fp32_->canvas_width();
      params->output_width_height[1] = stitcher_fp32_->canvas_height();
      g_print(
          "Stitched canvas size: %d x %d\n", (int)stitcher_fp32_->canvas_width(), (int)stitcher_fp32_->canvas_height());
      update_canvas_hints(stitcher_fp32_->canvas_width(), stitcher_fp32_->canvas_height());
    } else if (one_pass_mode_) {
      g_print("hmstitcher: deferring stitched canvas sizing until the first input batch\n");
    }
  }
  return Super::PreCapsInit(params);
}

absl::Status StitcherPriv::PostCapsInit(DSCustom_CreateParams* params) {
  return Super::PostCapsInit(params);
}

bool StitcherPriv::UsesRuntimeOutputSize() const {
  return one_pass_mode_ && (!canvas_width_hint_ || !canvas_height_hint_);
}

guint StitcherPriv::GetOutputBatchSize(guint input_batch_size, guint configured_batch_size) const {
  if (input_batch_size >= 2) {
    return std::max<guint>(1, input_batch_size / 2);
  }
  if (configured_batch_size >= 2) {
    return std::max<guint>(1, configured_batch_size / 2);
  }
  return 1;
}

absl::Status StitcherPriv::configure_one_pass_from_surfaces(
    hm::surface::Surface incoming_surface_left,
    hm::surface::Surface incoming_surface_right) {
  bool is_configured;
  HM_ASSIGN_OR_RETURN(is_configured, stitching::is_stitching_configured(config_file_));
  if (!is_configured) {
    if (!one_pass_mode_) {
      return absl::FailedPreconditionError("Stitching is not configured");
    }
    g_print("hmstitcher: configuring stitching in one-pass mode\n");
    if (!calibration_starts_from_control_points()) {
      report_calibration_progress("input", "started", "Waiting for synchronized frames from both cameras");
      report_calibration_progress("input", "complete", "Captured synchronized frames from both cameras");
    }
    if (!orientation_ran_) {
      // Configurator resolves auto camera orientation and synchronization before
      // constructing this pipeline. Explicit UI Left/Right roles are authoritative
      // inputs, so rerunning discovery here could overwrite them after the source
      // pads have already been assigned.
      if (!calibration_starts_from_control_points()) {
        report_calibration_progress("orientation", "started", "Loading the configured camera orientation");
        report_calibration_progress("orientation", "complete", "Camera orientation is configured");
      }
      orientation_ran_ = true;
    }
    absl::Status configure_status = stitching::configure_stitching(
        config_file_, incoming_surface_left, incoming_surface_right, calibration_invalidation_id_, [this] {
          return calibration_cancelled_.load(std::memory_order_acquire);
        });
    if (!configure_status.ok()) {
      std::cerr << configure_status << "\n" << std::flush;
      if (absl::IsCancelled(configure_status)) {
        // cancel-pending-work also requests base-class shutdown, so Cancelled
        // terminates the worker without pushing a second EOS or posting an
        // error on the pipeline bus.
        return configure_status;
      }
      return report_calibration_failure(configure_status);
    }
    configured_during_run_ = true;
  }

  bool stitcher_ready = false;
  {
    absl::MutexLock lk(&stitcher_mu_);
    stitcher_ready = has_stitcher();
  }
  if (!stitcher_ready) {
    absl::Status reload_status = reload_stitcher();
    if (!reload_status.ok()) {
      return configured_during_run_ ? report_calibration_failure(reload_status) : reload_status;
    }
  }
  {
    absl::MutexLock lk(&stitcher_mu_);
    stitcher_ready = has_stitcher();
  }
  if (!stitcher_ready) {
    const absl::Status status =
        absl::FailedPreconditionError("One-pass stitching configured but control masks could not be loaded");
    return configured_during_run_ ? report_calibration_failure(status) : status;
  }
  if (!canvas_width_hint_ || !canvas_height_hint_) {
    const absl::Status status = absl::FailedPreconditionError("One-pass stitching did not produce a canvas size");
    return configured_during_run_ ? report_calibration_failure(status) : status;
  }
  return absl::OkStatus();
}

absl::StatusOr<videoprep::RuntimeOutputSize> StitcherPriv::PrepareRuntimeOutputSize(
    NvDsBatchMeta* batch_meta,
    NvBufSurface* in_surface) {
  if (!UsesRuntimeOutputSize()) {
    return videoprep::RuntimeOutputSize{canvas_width_hint_, canvas_height_hint_, 0};
  }
  if (!batch_meta || !in_surface) {
    return absl::InvalidArgumentError("Cannot determine stitched canvas size without batch metadata and input surface");
  }
  if (in_surface->batchSize == 0 || in_surface->batchSize % 2 != 0 || in_surface->numFilled > in_surface->batchSize) {
    return absl::FailedPreconditionError(
        "Runtime stitching requires a positive even batch size and numFilled no greater than batchSize");
  }

  struct RuntimeFrameInfo {
    NvBufSurfaceParams* surface_params;
    size_t incoming_surface_index;
  };
  std::vector<RuntimeFrameInfo> runtime_frames;
  std::vector<RuntimeFrameKey> runtime_frame_keys;
  size_t surface_index = 0;
  size_t frame_meta_count = 0;
  size_t decoded_sequence_meta_count = 0;
  for (NvDsFrameMetaList* frame_meta_list = batch_meta->frame_meta_list; frame_meta_list != nullptr;
       frame_meta_list = frame_meta_list->next) {
    ++frame_meta_count;
    NvDsFrameMeta* frame_meta = (NvDsFrameMeta*)frame_meta_list->data;
    if (!frame_meta || frame_meta->num_surfaces_per_frame != 1) {
      return absl::FailedPreconditionError("Invalid frame metadata while determining stitched canvas size");
    }
    bool decoded_sequence_present = false;
    HM_RETURN_IF_ERROR(validate_decoder_mux_sequence(frame_meta, &decoded_sequence_present));
    decoded_sequence_meta_count += decoded_sequence_present ? 1 : 0;
    if (surface_index >= in_surface->numFilled) {
      continue;
    }
    auto* surface_params = &in_surface->surfaceList[surface_index];
    runtime_frames.push_back(
        RuntimeFrameInfo{
            .surface_params = surface_params,
            .incoming_surface_index = surface_index,
        });
    runtime_frame_keys.push_back(RuntimeFrameKey{frame_meta->frame_num, frame_meta->source_id});
    ++surface_index;
  }

  if (frame_meta_count != in_surface->numFilled || surface_index != in_surface->numFilled) {
    return absl::FailedPreconditionError("Input surface and frame metadata counts differ during runtime sizing");
  }
  if (decoded_sequence_meta_count != 0 && decoded_sequence_meta_count != frame_meta_count) {
    return absl::FailedPreconditionError(
        "Lossless stitching decode/mux invariant failed: decoded-frame sequence metadata is missing from part of the "
        "runtime-sizing batch");
  }
  if (require_decoded_frame_sequence_meta_ && decoded_sequence_meta_count != frame_meta_count) {
    return absl::FailedPreconditionError(
        "Lossless stitching decode/mux invariant failed: decoded-frame sequence metadata is required on every "
        "runtime-sizing frame");
  }
  const EosSnapshot eos_snapshot = snapshot_eos_for_surface(in_surface);
  absl::StatusOr<std::pair<size_t, size_t>> selected_pair =
      select_runtime_stitch_pair(runtime_frame_keys, eos_snapshot.source_ids, eos_snapshot.pipeline_eos_seen);
  if (!selected_pair.ok()) {
    return selected_pair.status();
  }
  const RuntimeFrameInfo& frame_info_left = runtime_frames[selected_pair->first];
  const RuntimeFrameInfo& frame_info_right = runtime_frames[selected_pair->second];

#ifdef __aarch64__
  hm::surface::EglSurfaceMapper incoming_left_elg_surface_mapper(
      in_surface, frame_info_left.incoming_surface_index, /*read_only=*/true);
  HM_RETURN_IF_ERROR(to_status(incoming_left_elg_surface_mapper.status()));
  hm::surface::Surface incoming_surface_left = incoming_left_elg_surface_mapper.get_surface();
  hm::surface::EglSurfaceMapper incoming_right_elg_surface_mapper(
      in_surface, frame_info_right.incoming_surface_index, /*read_only=*/true);
  HM_RETURN_IF_ERROR(to_status(incoming_right_elg_surface_mapper.status()));
  hm::surface::Surface incoming_surface_right = incoming_right_elg_surface_mapper.get_surface();
#else
  hm::surface::Surface incoming_surface_left(frame_info_left.surface_params);
  hm::surface::Surface incoming_surface_right(frame_info_right.surface_params);
#endif

  HM_RETURN_IF_ERROR(configure_one_pass_from_surfaces(incoming_surface_left, incoming_surface_right));
  return videoprep::RuntimeOutputSize{
      canvas_width_hint_, canvas_height_hint_, GetOutputBatchSize(in_surface->batchSize, 0)};
}

bool StitcherPriv::SetProperty(const Property& prop) {
  if (prop.key == "left-frame-offset-ns") {
    left_frame_offset_ns_ = std::atol(prop.value.c_str());
  } else if (prop.key == "right-frame-offset-ns") {
    right_frame_offset_ns_ = std::atol(prop.value.c_str());
  } else if (prop.key == "configure-only") {
    configure_only_ = !!std::atol(prop.value.c_str());
  } else if (prop.key == "one-pass-mode") {
    one_pass_mode_ = !!std::atol(prop.value.c_str());
  } else if (prop.key == "force-scoreboard-config" || prop.key == "force_scoreboard_config") {
    force_scoreboard_config_ = !!std::atol(prop.value.c_str());
  } else if (prop.key == "show") {
    show_ = !!std::atol(prop.value.c_str());
  } else if (
      prop.key == "stitch-auto-adjust-exposure" || prop.key == "stitch_auto_adjust_exposure" ||
      prop.key == "match-exposure" || prop.key == "match_exposure") {
    match_exposure_ = !!std::atol(prop.value.c_str());
  } else if (prop.key == "minimize-blend" || prop.key == "minimize_blend") {
    minimize_blend_ = !!std::atol(prop.value.c_str());
  } else if (prop.key == "require-decoded-frame-sequence-meta" || prop.key == "require_decoded_frame_sequence_meta") {
    require_decoded_frame_sequence_meta_ = !!std::atol(prop.value.c_str());
  } else if (prop.key == "cancel-pending-work") {
    calibration_cancelled_.store(true, std::memory_order_release);
    RequestShutdown();
  } else if (prop.key == "calibration-run-generation") {
    calibration_run_generation_ = prop.value;
  } else if (
      prop.key == "stitch-compute-precision" || prop.key == "stitch_compute_precision" ||
      prop.key == "stitcher-compute-precision" || prop.key == "stitcher_compute_precision") {
    const std::string value = normalized_property_value(prop.value);
    StitchComputePrecision requested_precision;
    if (value == "fp32" || value == "float32") {
      requested_precision = StitchComputePrecision::kFp32;
    } else if (value == "fp16" || value == "float16" || value == "half") {
      requested_precision = StitchComputePrecision::kFp16;
    } else {
      std::cerr << "Invalid stitch compute precision: " << prop.value << std::endl;
      return false;
    }
    absl::MutexLock lk(&stitcher_mu_);
    if (has_stitcher() && requested_precision != stitch_compute_precision_) {
      std::cerr << "Cannot change stitch compute precision after stitcher initialization" << std::endl;
      return false;
    }
    stitch_compute_precision_ = requested_precision;
  } else if (
      prop.key == "post-stitch-rotate-degrees" || prop.key == "post_stitch_rotate_degrees" ||
      prop.key == "stitch-rotate-degrees" || prop.key == "stitch_rotate_degrees") {
    double parsed_rotation = 0.0;
    if (!parse_finite_double(prop.value, parsed_rotation)) {
      std::cerr << "Invalid post-stitch rotation value: " << prop.value << std::endl;
      return false;
    }
    post_stitch_rotate_degrees_.store(parsed_rotation, std::memory_order_relaxed);
  }
  return true;
}

void StitcherPriv::release_rotation_scratch() {
  if (rotation_scratch_data_) {
    cudaFree(rotation_scratch_data_);
  }
  rotation_scratch_data_ = nullptr;
  rotation_scratch_pitch_ = 0;
  rotation_scratch_width_ = 0;
  rotation_scratch_height_ = 0;
  std::memset(&rotation_scratch_params_, 0, sizeof(rotation_scratch_params_));
}

absl::Status StitcherPriv::ensure_rotation_scratch(const hm::surface::Surface& surface, size_t width, size_t height) {
  if (surface->colorFormat != NVBUF_COLOR_FORMAT_RGBA) {
    return absl::FailedPreconditionError("post-stitch rotation only supports RGBA surfaces");
  }
  if (width > surface.width() || height > surface.height()) {
    return absl::FailedPreconditionError("post-stitch rotation dimensions exceed output surface");
  }
  if (rotation_scratch_data_ && rotation_scratch_width_ == width && rotation_scratch_height_ == height &&
      rotation_scratch_pitch_ == surface.pitch()) {
    return absl::OkStatus();
  }

  release_rotation_scratch();
  rotation_scratch_width_ = width;
  rotation_scratch_height_ = height;
  rotation_scratch_pitch_ = surface.pitch();
  XCUDA_RETURN_IF_ERROR(cudaMalloc(&rotation_scratch_data_, rotation_scratch_pitch_ * rotation_scratch_height_));

  std::memset(&rotation_scratch_params_, 0, sizeof(rotation_scratch_params_));
  rotation_scratch_params_.pitch = rotation_scratch_pitch_;
  rotation_scratch_params_.colorFormat = surface->colorFormat;
  rotation_scratch_params_.width = rotation_scratch_width_;
  rotation_scratch_params_.height = rotation_scratch_height_;
  rotation_scratch_params_.planeParams.num_planes = 1;
  rotation_scratch_params_.planeParams.width[0] = rotation_scratch_width_;
  rotation_scratch_params_.planeParams.height[0] = rotation_scratch_height_;
  rotation_scratch_params_.planeParams.pitch[0] = rotation_scratch_pitch_;
  rotation_scratch_params_.planeParams.psize[0] = rotation_scratch_pitch_ * rotation_scratch_height_;
  rotation_scratch_params_.planeParams.bytesPerPix[0] = 4;
  rotation_scratch_params_.dataSize = rotation_scratch_params_.planeParams.psize[0];
  rotation_scratch_params_.dataPtr = rotation_scratch_data_;
  rotation_scratch_params_.layout = NVBUF_LAYOUT_PITCH;
  return absl::OkStatus();
}

absl::Status StitcherPriv::apply_post_stitch_rotation(
    hm::surface::Surface surface,
    size_t width,
    size_t height,
    double post_stitch_rotate_degrees) {
  if (std::abs(post_stitch_rotate_degrees) < 1e-6) {
    return absl::OkStatus();
  }
  HM_RETURN_IF_ERROR(ensure_rotation_scratch(surface, width, height));

  hm::surface::Surface scratch_surface(&rotation_scratch_params_);
  NppStreamContext npp_stream_context;
  std::memset(&npp_stream_context, 0, sizeof(npp_stream_context));
  npp_stream_context.hStream = cuda_stream_;
  npp_stream_context.nStreamFlags = 0;
  npp_stream_context.nCudaDeviceId = m_gpuId;

  const double radians = -post_stitch_rotate_degrees * M_PI / 180.0;
  const double cos_angle = std::cos(radians);
  const double sin_angle = std::sin(radians);
  const double cx = (static_cast<double>(width) - 1.0) / 2.0;
  const double cy = (static_cast<double>(height) - 1.0) / 2.0;
  const double coeffs[2][3] = {
      {cos_angle, sin_angle, (1.0 - cos_angle) * cx - sin_angle * cy},
      {-sin_angle, cos_angle, sin_angle * cx + (1.0 - cos_angle) * cy},
  };
  const NppiSize image_size{static_cast<int>(width), static_cast<int>(height)};
  const NppiRect roi{0, 0, static_cast<int>(width), static_cast<int>(height)};

  XCUDA_RETURN_IF_ERROR(
      cudaMemsetAsync(scratch_surface.dataptr(), 0, scratch_surface.pitch() * scratch_surface.height(), cuda_stream_));
  XCUDA_RETURN_IF_ERROR(mapNppStatusToCudaError(nppiWarpAffineBack_8u_C4R_Ctx(
      surface.dataptr<Npp8u*>(),
      image_size,
      surface.pitch(),
      roi,
      scratch_surface.dataptr<Npp8u*>(),
      scratch_surface.pitch(),
      roi,
      coeffs,
      NPPI_INTER_LINEAR,
      npp_stream_context)));
  XCUDA_RETURN_IF_ERROR(cudaMemcpy2DAsync(
      surface.dataptr(),
      surface.pitch(),
      scratch_surface.dataptr(),
      scratch_surface.pitch(),
      width * surface.bytes_per_pixel(),
      height,
      cudaMemcpyDeviceToDevice,
      cuda_stream_));
  return absl::OkStatus();
}

struct ModifyBatchFrames {
  ModifyBatchFrames(NvDsBatchMeta* batch_meta, const std::vector<NvDsFrameMeta*>& remove) : batch_meta_(batch_meta) {
    // 1. Acquire the meta lock to ensure thread safety
    nvds_acquire_meta_lock(batch_meta);

    for (NvDsFrameMeta* f : remove) {
      nvds_remove_frame_meta_from_batch(batch_meta_, f);
    }
  }

  bool add_frame(const std::function<bool(NvDsFrameMeta* frame_meta)>& cb) {
    NvDsFrameMeta* frame_meta = nvds_acquire_frame_meta_from_pool(batch_meta_);
    if (!frame_meta) {
      return false;
    }
    if (!cb(frame_meta)) {
      return false;
    }
    nvds_add_frame_meta_to_batch(batch_meta_, frame_meta);
    return true;
  }
  ~ModifyBatchFrames() {
    // 5. Release the meta lock
    nvds_release_meta_lock(batch_meta_);
  }
  NvDsBatchMeta* batch_meta_;
};

absl::Status StitcherPriv::GenerateOutput(
    NvDsBatchMeta* batch_meta,
    NvBufSurface* in_surface,
    NvBufSurface* out_surface) {
  if (!batch_meta || !in_surface || !out_surface) {
    return absl::InvalidArgumentError("Stitcher GenerateOutput requires batch meta, input surface, and output surface");
  }
  if (in_surface->batchSize == 0 || in_surface->batchSize % 2 != 0 || in_surface->numFilled > in_surface->batchSize) {
    return absl::FailedPreconditionError(
        "Stitcher output requires a positive even batch size and numFilled no greater than batchSize");
  }
  // assert(in_surface->isContiguous);

  struct FrameInfo {
    NvBufSurfaceParams* surface_params;
    // This is the actual frame meta for this surface
    NvDsFrameMeta* frame_meta;
    // This is the frame meta that we'll keep for the final stitched frame (we'll discard on of them)
    // NvDsFrameMeta* persistent_frame_meta;
    size_t incoming_surface_index;
  };

  //  frame_number -> source_id -> NvBufSurfaceParams*
  std::map<int, std::map<int, FrameInfo>> frame_source_surfaces;

  const EosSnapshot eos_snapshot = snapshot_eos_for_surface(in_surface);

  std::map<guint, NvDsFrameMeta*> source_frame_metas;
  std::set<guint> observed_source_ids;

  guint min_source_id = std::numeric_limits<guint>::max();
  guint max_source_id = 0;
  size_t surface_index = 0;
  size_t frame_meta_count = 0;
  size_t duplicate_frame_sources = 0;
  size_t invalid_surfaces_per_frame = 0;
  size_t decoded_sequence_meta_count = 0;
  for (NvDsFrameMetaList* frame_meta_list = batch_meta->frame_meta_list; frame_meta_list != nullptr;
       frame_meta_list = frame_meta_list->next) {
    ++frame_meta_count;
    NvDsFrameMeta* frame_meta = (NvDsFrameMeta*)frame_meta_list->data;
    if (!frame_meta) {
      ++invalid_surfaces_per_frame;
      continue;
    }
    if (frame_meta->num_surfaces_per_frame != 1) {
      ++invalid_surfaces_per_frame;
    }
    bool decoded_sequence_present = false;
    HM_RETURN_IF_ERROR(validate_decoder_mux_sequence(frame_meta, &decoded_sequence_present));
    decoded_sequence_meta_count += decoded_sequence_present ? 1 : 0;
    if (surface_index >= in_surface->numFilled) {
      continue;
    }
    auto* surface_params = &in_surface->surfaceList[surface_index];
    // assert(seen_surface_indexes.emplace(frame_meta->surface_index).second);
    // std::cout << "source_id=" << frame_meta->source_id << ", frame_num=" << frame_meta->frame_num << std::endl;
    auto& frame_sources = frame_source_surfaces[frame_meta->frame_num];
    min_source_id = std::min(min_source_id, frame_meta->source_id);
    max_source_id = std::max(max_source_id, frame_meta->source_id);
    observed_source_ids.insert(frame_meta->source_id);

    source_frame_metas.emplace(frame_meta->source_id, frame_meta);

    const FrameInfo frame_info{
        .surface_params = surface_params,
        .frame_meta = frame_meta,
        .incoming_surface_index = surface_index,
    };
    const bool inserted = frame_sources.emplace(frame_meta->source_id, frame_info).second;
    if (!inserted) {
      ++duplicate_frame_sources;
    }
    ++surface_index;
  }
  if (decoded_sequence_meta_count != 0 && decoded_sequence_meta_count != frame_meta_count) {
    return absl::FailedPreconditionError(
        "Lossless stitching decode/mux invariant failed: decoded-frame sequence metadata is missing from part of the "
        "batch");
  }
  if (require_decoded_frame_sequence_meta_ && decoded_sequence_meta_count != frame_meta_count) {
    return absl::FailedPreconditionError(
        "Lossless stitching decode/mux invariant failed: decoded-frame sequence metadata is required on every frame");
  }

  std::set<guint> expected_source_ids;
  size_t incomplete_frame_groups = 0;
  size_t inconsistent_source_groups = 0;
  std::set<guint> missing_eos_source_ids;
  for (const auto& [frame_number, source_to_surface] : frame_source_surfaces) {
    (void)frame_number;
    if (source_to_surface.size() != 2) {
      ++incomplete_frame_groups;
      std::set<guint> group_missing_eos_source_ids;
      for (guint eos_source_id : eos_snapshot.source_ids) {
        if (!source_to_surface.count(eos_source_id)) {
          group_missing_eos_source_ids.insert(eos_source_id);
        }
      }
      missing_eos_source_ids.insert(group_missing_eos_source_ids.begin(), group_missing_eos_source_ids.end());
      continue;
    }

    std::set<guint> group_source_ids;
    for (const auto& [source_id, frame_info] : source_to_surface) {
      (void)frame_info;
      group_source_ids.insert(source_id);
    }
    if (expected_source_ids.empty()) {
      expected_source_ids = group_source_ids;
    } else if (group_source_ids != expected_source_ids) {
      ++inconsistent_source_groups;
      for (guint expected_source_id : expected_source_ids) {
        if (!group_source_ids.count(expected_source_id) && eos_snapshot.source_ids.count(expected_source_id)) {
          missing_eos_source_ids.insert(expected_source_id);
        }
      }
    }
  }
  if (observed_source_ids.size() < 2) {
    for (guint eos_source_id : eos_snapshot.source_ids) {
      if (!observed_source_ids.count(eos_source_id)) {
        missing_eos_source_ids.insert(eos_source_id);
      }
    }
  }
  const bool invalid_frame_sequence = (in_surface->batchSize % 2 != 0) || (in_surface->numFilled % 2 != 0) ||
      surface_index != in_surface->numFilled || frame_meta_count != in_surface->numFilled ||
      duplicate_frame_sources > 0 || invalid_surfaces_per_frame > 0 || observed_source_ids.size() != 2 ||
      frame_source_surfaces.size() != in_surface->numFilled / 2 || incomplete_frame_groups > 0 ||
      inconsistent_source_groups > 0;
  if (invalid_frame_sequence) {
    std::string reason = "invalid_frame_sequence";
    if (in_surface->numFilled % 2 != 0) {
      reason = "odd_num_filled";
    } else if (duplicate_frame_sources > 0) {
      reason = "duplicate_frame_source";
    } else if (incomplete_frame_groups > 0) {
      reason = "incomplete_frame_group";
    } else if (inconsistent_source_groups > 0 || observed_source_ids.size() != 2) {
      reason = "inconsistent_sources";
    } else if (frame_meta_count != in_surface->numFilled || surface_index != in_surface->numFilled) {
      reason = "surface_metadata_mismatch";
    }
    const bool valid_surface_envelope =
        in_surface->batchSize > 0 && in_surface->batchSize % 2 == 0 && in_surface->numFilled <= in_surface->batchSize;
    const bool metadata_valid_unpairable_batch = valid_surface_envelope && in_surface->numFilled > 0 &&
        frame_meta_count == in_surface->numFilled && surface_index == in_surface->numFilled &&
        duplicate_frame_sources == 0 && invalid_surfaces_per_frame == 0 && observed_source_ids.size() <= 2;
    std::set<guint> observed_or_eos_source_ids = observed_source_ids;
    observed_or_eos_source_ids.insert(eos_snapshot.source_ids.begin(), eos_snapshot.source_ids.end());
    const bool source_eos_explains_unpairable_batch =
        metadata_valid_unpairable_batch && !eos_snapshot.source_ids.empty() && observed_or_eos_source_ids.size() <= 2;
    const absl::Status mismatch_status = frame_sequence_mismatch_status(
        reason,
        frame_source_surfaces.size(),
        in_surface->numFilled,
        frame_meta_count,
        duplicate_frame_sources,
        incomplete_frame_groups,
        inconsistent_source_groups,
        invalid_surfaces_per_frame,
        observed_source_ids,
        missing_eos_source_ids,
        eos_snapshot.source_ids,
        eos_snapshot.pipeline_eos_seen && metadata_valid_unpairable_batch,
        eos_snapshot.pipeline_eos_seen);
    if (source_eos_explains_unpairable_batch) {
      // URI-MULTIPLE suppresses chapter-boundary EOS before nvstreammux. A source EOS observed here therefore means
      // that a camera has permanently ended, so stop stitched output instead of discarding frames and continuing.
      return absl::CancelledError(mismatch_status.message());
    }
    return mismatch_status;
  }

  std::vector<gint> complete_frame_numbers;
  complete_frame_numbers.reserve(frame_source_surfaces.size());
  for (const auto& [frame_number, source_to_surface] : frame_source_surfaces) {
    (void)source_to_surface;
    complete_frame_numbers.push_back(frame_number);
  }
  HM_ASSIGN_OR_RETURN(
      last_stitched_frame_num_, validate_stitch_frame_continuity(complete_frame_numbers, last_stitched_frame_num_));

  assert(cuda_stream_);

  HM_RETURN_IF_ERROR(to_status(cudaSetDevice(m_gpuId)));

  // We will have this many output frames
  const size_t batch_size = frame_source_surfaces.size();
  HM_RETURN_IF_ERROR(prepare_stitch_output_surface(out_surface, batch_size));

  if (log_batches_enabled()) {
    g_print(
        "hmstitcher batch in: surface batchSize=%u numFilled=%u frame_meta_count=%u frame_meta_list_len=%u "
        "frame_pairs=%zu output_capacity=%u\n",
        in_surface->batchSize,
        in_surface->numFilled,
        batch_meta->num_frames_in_batch,
        g_list_length(batch_meta->frame_meta_list),
        batch_size,
        out_surface->batchSize);
  }

  std::vector<NvDsFrameMeta*> remove_frame_metas;
  remove_frame_metas.reserve(in_surface->batchSize / 2);

  // for (size_t batch_nr = 0; batch_nr < batch_size; batch_nr++) {
  size_t out_surface_index = 0;
  for (auto frame_iter = frame_source_surfaces.begin(), frame_end = frame_source_surfaces.end();
       frame_iter != frame_end;
       ++frame_iter, ++out_surface_index) {
    // const int frame_num = frame_iter->first;
    // std::cout << "frame_num=" << frame_num << std::endl;
    auto& source_to_surface = frame_iter->second;
    assert(source_to_surface.size() == 2);

    const FrameInfo& frame_info_left = source_to_surface.begin()->second;
    const FrameInfo& frame_info_right = source_to_surface.rbegin()->second;
    // This tests the assumption that the source ids come in sorted

    NvBufSurfaceParams* output_params = &out_surface->surfaceList[out_surface_index];

    // render("left", frame_info_left.surface_params, cuda_stream_);
    // render("right", frame_info_right.surface_params, cuda_stream_);

    NvDsFrameMeta* reuse_frame_meta{nullptr};
    assert(source_frame_metas.size() == 2);
    if (!left_frame_offset_ns_) {
      // left frame has correct timestamps
      reuse_frame_meta = frame_info_left.frame_meta;
      remove_frame_metas.emplace_back(frame_info_right.frame_meta);
    } else {
      // Can't get renderer to pick up source-id 1 automatically, even if I change in frame meta

      // right frame has correct timestamps
      assert(!right_frame_offset_ns_);
      reuse_frame_meta = frame_info_right.frame_meta;
      remove_frame_metas.emplace_back(frame_info_left.frame_meta);
    }

#ifdef __aarch64__
    hm::surface::EglSurfaceMapper incoming_left_elg_surface_mapper(
        in_surface, frame_info_left.incoming_surface_index, /*read_only=*/true);
    HM_RETURN_IF_ERROR(to_status(incoming_left_elg_surface_mapper.status()));
    hm::surface::Surface incoming_surface_left = incoming_left_elg_surface_mapper.get_surface();
    hm::surface::EglSurfaceMapper incoming_right_elg_surface_mapper(
        in_surface, frame_info_right.incoming_surface_index, /*read_only=*/true);
    HM_RETURN_IF_ERROR(to_status(incoming_right_elg_surface_mapper.status()));
    hm::surface::Surface incoming_surface_right = incoming_right_elg_surface_mapper.get_surface();
    hm::surface::EglSurfaceMapper outgoing_elg_surface_mapper(out_surface, out_surface_index, /*read_only=*/false);
    HM_RETURN_IF_ERROR(to_status(outgoing_elg_surface_mapper.status()));
    hm::surface::Surface outgoing_surface = outgoing_elg_surface_mapper.get_surface();
#else
    hm::surface::Surface incoming_surface_left(frame_info_left.surface_params);
    hm::surface::Surface incoming_surface_right(frame_info_right.surface_params);
    hm::surface::Surface outgoing_surface(&out_surface->surfaceList[out_surface_index]);
#endif

    // Maybe configure stitching with these frames
    if (!process_pass_++) {
      bool is_configured;
      HM_ASSIGN_OR_RETURN(is_configured, stitching::is_stitching_configured(config_file_));
      if (!is_configured || configure_only_) {
        if (one_pass_mode_ && !is_configured) {
          HM_RETURN_IF_ERROR(configure_one_pass_from_surfaces(incoming_surface_left, incoming_surface_right));
        } else if (!configure_only_) {
          return absl::FailedPreconditionError("Stitching is not configured");
        } else {
          if (!orientation_ran_) {
            absl::Status orientation_status =
                stitching::configure_orientation(config_file_, calibration_invalidation_id_, [this] {
                  return calibration_cancelled_.load(std::memory_order_acquire);
                });
            if (!orientation_status.ok()) {
              std::cerr << orientation_status << "\n" << std::flush;
              return orientation_status;
            }
            orientation_ran_ = true;
          }
          absl::Status configure_status = stitching::configure_stitching(
              config_file_, incoming_surface_left, incoming_surface_right, calibration_invalidation_id_, [this] {
                return calibration_cancelled_.load(std::memory_order_acquire);
              });
          if (!configure_status.ok()) {
            std::cerr << configure_status << "\n" << std::flush;
            return to_status(CudaStatus(
                cudaError_t::cudaErrorLaunchFailure, (std::stringstream() << configure_status.message()).str()));
          }
          // return absl::CancelledError("Stitching has been configured");
          if (!post_force_pipeline_eos(GST_ELEMENT(m_element))) {
            std::cerr << "Failed to post pipeline EOS, returning an error to stop the pipeline";
            return absl::CancelledError("Stitching has been configured");
          }
        }
      } else if (one_pass_mode_) {
        // Masks existed but we had no stitcher due to earlier failure; retry once in one-pass mode.
        bool needs_reload = false;
        {
          absl::MutexLock lk(&stitcher_mu_);
          needs_reload = !has_stitcher();
        }
        if (needs_reload) {
          absl::Status reload_status = reload_stitcher();
          if (!reload_status.ok()) {
            return reload_status;
          }
        }
        {
          absl::MutexLock lk(&stitcher_mu_);
          if (configured_during_run_ && !has_stitcher()) {
            return absl::FailedPreconditionError("One-pass stitching configured but control masks could not be loaded");
          }
        }
      }
    }

    if (canvas_width_hint_ && canvas_height_hint_) {
      if (canvas_width_hint_ > outgoing_surface.width() || canvas_height_hint_ > outgoing_surface.height()) {
        return absl::FailedPreconditionError("Output surface is smaller than expected stitched canvas");
      }
      output_params->width = canvas_width_hint_;
      output_params->height = canvas_height_hint_;
    }

    // auto osw = outgoing_surface.width();
    // auto cvw = stitcher_->canvas_width();
    //  assert(outgoing_surface.width() == (uint32_t)stitcher_->canvas_width());
    //  assert(outgoing_surface.height() == (uint32_t)stitcher_->canvas_height());
    hm::CudaMat<uchar4> left(
        hm::SurfaceInfo{
            .width = (int)incoming_surface_left.width(),
            .height = (int)incoming_surface_left.height(),
            .pitch = (int)incoming_surface_left.pitch(),
            .data_ptr = incoming_surface_left.dataptr(),
        },
        /*batch_size=*/1);
    hm::CudaMat<uchar4> right(
        hm::SurfaceInfo{
            .width = (int)incoming_surface_right.width(),
            .height = (int)incoming_surface_right.height(),
            .pitch = (int)incoming_surface_right.pitch(),
            .data_ptr = incoming_surface_right.dataptr(),
        },
        /*batch_size=*/1);
    auto canvas = std::make_unique<hm::CudaMat<uchar4>>(
        hm::SurfaceInfo{
            .width = (int)output_params->width, // egl mapping may have slightly different width
            .height = (int)output_params->height,
            .pitch = (int)outgoing_surface.pitch(), // pitch may be egl image pitch
            .data_ptr = outgoing_surface.dataptr(),
        },
        /*batch_size=*/1);

    // render("left", left_params, cuda_stream_);
    // render("right", right_params, cuda_stream_);
    // Why suddenly now I need to clear the canvas?

    assert(cuda_stream_);

    if (stitcher_fp16_) {
      HM_RETURN_IF_ERROR(to_status(cudaMemsetAsync(
          canvas->data_raw(), 0, canvas->height() * canvas->pitch() * canvas->batch_size(), cuda_stream_)));
      HM_CUDA_ASSIGN_OR_RETURN(canvas, stitcher_fp16_->process(left, right, cuda_stream_, std::move(canvas)));
    } else if (stitcher_fp32_) {
      HM_RETURN_IF_ERROR(to_status(cudaMemsetAsync(
          canvas->data_raw(), 0, canvas->height() * canvas->pitch() * canvas->batch_size(), cuda_stream_)));
      HM_CUDA_ASSIGN_OR_RETURN(canvas, stitcher_fp32_->process(left, right, cuda_stream_, std::move(canvas)));
    } else {
      // Gray image
      HM_RETURN_IF_ERROR(to_status(cudaMemsetAsync(
          canvas->data_raw(), 128, canvas->height() * canvas->pitch() * canvas->batch_size(), cuda_stream_)));
    }
    NvBufSurfaceParams logical_output_params = *outgoing_surface.get_mutable();
    logical_output_params.width = canvas->width();
    logical_output_params.height = canvas->height();
    logical_output_params.planeParams.width[0] = canvas->width();
    logical_output_params.planeParams.height[0] = canvas->height();
    hm::surface::Surface logical_output_surface(&logical_output_params);

    const double applied_post_stitch_rotation = post_stitch_rotate_degrees_.load(std::memory_order_relaxed);
    HM_RETURN_IF_ERROR(apply_post_stitch_rotation(
        logical_output_surface, canvas->width(), canvas->height(), applied_post_stitch_rotation));
    std::string hugin_generation;
    {
      absl::MutexLock lk(&stitcher_mu_);
      hugin_generation = hugin_generation_id_;
    }
    std::string output_generation;
    HM_ASSIGN_OR_RETURN(
        output_generation, stitching::stitched_output_generation_id(hugin_generation, applied_post_stitch_rotation));
    const std::string completion_scope =
        calibration_completion_scope(output_generation, calibration_invalidation_id_, calibration_run_generation_);

    if (one_pass_mode_ && !field_mask_attempted_) {
      field_mask_attempted_ = true;
      bool mask_configured = stitching::is_field_mask_configured(config_file_, output_generation);
      OnePassCalibrationProgressPlan progress = one_pass_calibration_progress_plan(
          configured_during_run_,
          mask_configured,
          /*report_latched=*/false,
          process_calibration_completion_latch.delivered(completion_scope));
      if (progress.report) {
        report_calibration_progress("rink-mask", "started", "Looking for the ice surface in the stitched panorama");
      }
      if (progress.create_mask) {
        absl::Status mask_status = stitching::create_field_mask(
            config_file_, logical_output_surface, output_generation, calibration_invalidation_id_, [this] {
              return calibration_cancelled_.load(std::memory_order_acquire);
            });
        if (!mask_status.ok()) {
          std::cerr << "Failed to create field mask: " << mask_status << "\n" << std::flush;
          calibration_completion_reported_ = true;
          if (absl::IsCancelled(mask_status)) {
            return mask_status;
          }
          return report_calibration_failure(mask_status);
        } else {
          mask_configured = true;
        }
      }
      progress = one_pass_calibration_progress_plan(
          configured_during_run_,
          mask_configured,
          /*report_latched=*/progress.report,
          process_calibration_completion_latch.delivered(completion_scope));
      calibration_completion_ready_ = progress.complete;
    }
    if (one_pass_mode_ && calibration_completion_ready_ && !calibration_completion_reported_) {
      if (g_getenv("HM_TEST_SUPPRESS_STITCHING_CALIBRATION_COMPLETION")) {
        calibration_completion_reported_ = true;
        g_print("hmstitcher: suppressing calibration completion for lifecycle test\n");
      } else if (owner_element_) {
        const bool delivered = gst_element_post_message(
            owner_element_,
            gst_message_new_element(
                GST_OBJECT(owner_element_),
                gst_structure_new(
                    "hstream-stitching-calibration-complete",
                    "output-generation",
                    G_TYPE_STRING,
                    output_generation.c_str(),
                    "calibration-scope",
                    G_TYPE_STRING,
                    completion_scope.c_str(),
                    nullptr)));
        if (delivered) {
          calibration_completion_reported_ = true;
          if (process_calibration_completion_latch.try_begin_delivery(completion_scope)) {
            report_calibration_progress("rink-mask", "complete", "Ice surface calibration is ready");
            report_calibration_progress("calibration", "complete", "Stitching calibration is complete");
            g_print("hmstitcher: one-pass stitching configuration complete\n");
            std::fflush(stdout);
            process_calibration_completion_latch.finish_delivery(completion_scope, /*delivered=*/true);
          }
        }
      }
    }

#ifdef HAS_NVDS_CUSTOMUSERMETA
    stitching::StitchedOutputGenerationPayload::create_and_add<stitching::StitchedOutputGenerationPayload>(
        reuse_frame_meta, output_generation);
#endif

    if (show_) {
      render("HM Stitcher (LEFT)", incoming_surface_left, cuda_stream_);
      render("HM Stitcher (RIGHT)", incoming_surface_right, cuda_stream_);
      render("HM Stitcher", logical_output_surface, cuda_stream_);
    }

    // render("canvas", output_params, cuda_stream_);
    ++out_surface->numFilled;
    // Both should have the same 'persistent_frame_meta'
    // TODO: Should we do this later under a batch meta lock?
    // ModifyBatchFrames frame_adder(batch_meta, remove_frame_metas);

    reuse_frame_meta->source_frame_width = reuse_frame_meta->pipeline_width = canvas->width();
    reuse_frame_meta->source_frame_height = reuse_frame_meta->pipeline_height = canvas->height();
    reuse_frame_meta->num_surfaces_per_frame = 1;
    // This transform reduces two input sources (left/right) into a single stitched stream.
    // Downstream elements (nvstreamdemux, perf measurement, sinks) route using `pad_index`/`batch_id`.
    // When we reuse the right frame meta, `pad_index` and `batch_id` would otherwise remain `1` and route the stitched
    // output to source 1 (often unlinked), resulting in a "gray box" / no visible output.
    //
    // Normalize all stitched output frames to the first stream (min pad index) and to the current output surface slot.
    const guint out_pad_index = std::min(frame_info_left.frame_meta->pad_index, frame_info_right.frame_meta->pad_index);
    reuse_frame_meta->pad_index = out_pad_index;
    reuse_frame_meta->batch_id = static_cast<guint>(out_surface_index);
    reuse_frame_meta->surface_index = 0;
    reuse_frame_meta->source_id = min_source_id;
  }

  if (out_surface->numFilled) {
    // batch_meta->num_frames_in_batch /= 2;
    ModifyBatchFrames modifier(batch_meta, remove_frame_metas);
    // batch_meta->max_frames_in_batch /= 2;
    // batch_meta->frame_meta_pool->max_elements_in_pool /= 2;
    batch_meta->max_frames_in_batch = batch_meta->num_frames_in_batch;
    // assert(batch_meta->max_frames_in_batch); // make sure we didnt do too many times and make it 0
    HM_RETURN_IF_ERROR(to_status(cudaStreamSynchronize(cuda_stream_)));
  }
  if (log_batches_enabled()) {
    g_print(
        "hmstitcher batch out: surface batchSize=%u numFilled=%u frame_meta_count=%u frame_meta_list_len=%u\n",
        out_surface->batchSize,
        out_surface->numFilled,
        batch_meta->num_frames_in_batch,
        g_list_length(batch_meta->frame_meta_list));
  }
  // videoprep::videoprep_add_surface_meta(videoprep->out_gst_buf, out_surface->numFilled, videoprep->source_id);
  return absl::OkStatus();
}

} // namespace stitcher
} // namespace hm
