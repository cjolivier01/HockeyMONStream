#include "hstream/src/gst-plugins/gst-player-analytics/PlayerAnalyticsProcessor.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "hstream/src/gst-plugins/gst-player-analytics/BorrowedImage.h"
#include "hstream/src/gst-plugins/gst-player-analytics/FrameWorkPlan.h"
#include "hstream/src/libs/player_analytics/FrameMeta.h"
#include "hstream/src/libs/player_analytics/TemporalState.h"
#include "hstream/src/libs/player_analytics/runtime/NativeEngine.h"
#include "hstream/src/libs/player_analytics/runtime/PoseGpu.h"
#include "hstream/src/libs/player_analytics/runtime/SemanticRoi.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

// Keep this metadata/GPU adapter independent of display/OpenGL status helpers.
#define PA_RETURN_IF_ERROR(expression)        \
  do {                                        \
    const absl::Status status = (expression); \
    if (!status.ok())                         \
      return status;                          \
  } while (false)

namespace hm::player_analytics {
namespace {
absl::Status Cuda(cudaError_t error) {
  return error == cudaSuccess ? absl::OkStatus() : absl::InternalError(cudaGetErrorString(error));
}

// Must be declared after the borrowed image so failure/exception fences execute
// before EGL imports or upstream surfaces are released.
struct StreamFence {
  cudaStream_t stream;
  bool pending{false};
  absl::Status Finish() {
    const auto error = cudaStreamSynchronize(stream);
    pending = error != cudaSuccess;
    return Cuda(error);
  }
  ~StreamFence() {
    if (pending) {
      const auto error = cudaStreamSynchronize(stream);
      if (error != cudaSuccess)
        std::fprintf(stderr, "Player analytics error-path fence failed: %s\n", cudaGetErrorString(error));
    }
  }
};

void Cleanup(cudaError_t error, const char* operation) noexcept {
  if (error != cudaSuccess)
    std::fprintf(stderr, "Player analytics %s failed: %s\n", operation, cudaGetErrorString(error));
}

bool ValidBox(const Box& box) {
  return std::isfinite(box.left) && std::isfinite(box.top) && std::isfinite(box.width) && std::isfinite(box.height) &&
      std::abs(box.left) <= 1000000 && std::abs(box.top) <= 1000000 && box.width > 0 && box.height > 0 &&
      box.width <= 1000000 && box.height <= 1000000;
}
} // namespace

struct PlayerAnalyticsProcessor::Impl {
  struct ActionState {
    ActionHistory history;
    ActionLabelState label;
    uint64_t last_pose{kInvalidTime};
    uint64_t attempted_window{kInvalidTime};
    void Reset() noexcept {
      history.Reset();
      label.Reset();
      last_pose = attempted_window = kInvalidTime;
    }
  };
  struct Evidence {
    uint64_t incarnation{0};
    std::unique_ptr<JerseyConsensus> jersey;
    std::unique_ptr<ActionState> action;
  };
  struct Source {
    explicit Source(const Config& config) : scheduler(config.maximum_tracks, config.track_retention_ns) {}
    TrackScheduler scheduler;
    std::map<uint64_t, Evidence> evidence;
    unsigned scheduling_cursor{0};
    void Reset() noexcept {
      scheduler.Reset();
      evidence.clear();
      scheduling_cursor = 0;
    }
    std::string geometry;
    uint64_t epoch{1};
    uint64_t geometry_token{1};
    uint64_t last_pts{kInvalidTime};
    int64_t last_frame{-1};
    uint32_t source_id{0};
    uint32_t width{0};
    uint32_t height{0};
  };

  Config config;
  int gpu_id{0};
  cudaStream_t stream{nullptr};
  std::unique_ptr<NativeEngine> pose;
  std::unique_ptr<NativeEngine> jersey;
  std::unique_ptr<NativeEngine> action;
  JerseyVocabulary jersey_vocabulary;
  void* jersey_scratch{nullptr};
  size_t jersey_scratch_bytes{0};
  JerseyObservation* device_jerseys{nullptr};
  JerseyObservation* host_jerseys{nullptr};
  ActionObservation* device_actions{nullptr};
  ActionObservation* host_actions{nullptr};
  float* host_action_tensor{nullptr};
  PoseAffine* device_affines{nullptr};
  Pose* device_poses{nullptr};
  PoseAffine* host_affines{nullptr};
  Pose* host_poses{nullptr};
  std::map<uint32_t, Source> sources;
  AnalyticsCounters counters;
  std::array<uint64_t, 3> intervals{};
  uint64_t next_epoch{1};
  std::atomic<uint64_t> publication_epoch{1};
  std::atomic<bool> flushing{false};
  // Protect only immutable attachment and epoch retirement, never GPU work.
  std::mutex publication_mutex;

  bool Cancelled(uint64_t epoch) const noexcept {
    return flushing.load(std::memory_order_acquire) || publication_epoch.load(std::memory_order_acquire) != epoch;
  }

  ~Impl() {
    if (!stream)
      return;
    Cleanup(cudaSetDevice(gpu_id), "cleanup device selection");
    Cleanup(cudaStreamSynchronize(stream), "cleanup stream fence");
    pose.reset();
    jersey.reset();
    action.reset();
    if (jersey_scratch)
      Cleanup(cudaFree(jersey_scratch), "jersey scratch release");
    if (device_jerseys)
      Cleanup(cudaFree(device_jerseys), "jersey release");
    if (host_jerseys)
      Cleanup(cudaFreeHost(host_jerseys), "host jersey release");
    if (device_actions)
      Cleanup(cudaFree(device_actions), "action release");
    if (host_actions)
      Cleanup(cudaFreeHost(host_actions), "host action release");
    if (host_action_tensor)
      Cleanup(cudaFreeHost(host_action_tensor), "host action tensor release");
    if (device_affines)
      Cleanup(cudaFree(device_affines), "affine release");
    if (device_poses)
      Cleanup(cudaFree(device_poses), "pose release");
    if (host_affines)
      Cleanup(cudaFreeHost(host_affines), "host affine release");
    if (host_poses)
      Cleanup(cudaFreeHost(host_poses), "host pose release");
    Cleanup(cudaStreamDestroy(stream), "stream destruction");
  }

  absl::Status Frame(NvBufSurface* surface, NvDsFrameMeta* frame, uint64_t publication) {
    ++counters.frames;
    auto source_it = sources.find(frame->pad_index);
    if (source_it == sources.end()) {
      if (sources.size() >= 64)
        return absl::ResourceExhaustedError("Player analytics stream capacity exceeded");
      source_it = sources.try_emplace(frame->pad_index, config).first;
      source_it->second.epoch = next_epoch++;
    }
    Source& source = source_it->second;
    const auto* generation = stitching::find_stitched_output_generation_meta(frame);
    const std::string_view geometry = generation ? generation->generation() : std::string_view();
    if (geometry.size() > 4096)
      return absl::InvalidArgumentError("Player analytics geometry identity exceeds its bound");
    const bool geometry_changed = source.geometry != geometry || source.width != frame->source_frame_width ||
        source.height != frame->source_frame_height || source.source_id != frame->source_id;
    if (geometry_changed || (source.last_pts != kInvalidTime && frame->buf_pts < source.last_pts) ||
        (source.last_frame >= 0 && frame->frame_num < source.last_frame)) {
      source.epoch = next_epoch++;
      if (geometry_changed)
        ++source.geometry_token;
      source.Reset();
    }
    if (source.geometry != geometry)
      source.geometry = geometry;
    source.width = frame->source_frame_width;
    source.height = frame->source_frame_height;
    source.source_id = frame->source_id;
    source.last_frame = frame->frame_num;
    source.last_pts = frame->buf_pts;
    if (frame->buf_pts == kInvalidTime) {
      ++counters.invalid_time;
      source.epoch = next_epoch++;
      source.Reset();
      return absl::OkStatus();
    }
    if (source.width == 0 || source.height == 0 || source.width > 1000000 || source.height > 1000000)
      return absl::InvalidArgumentError("Player analytics requires valid metadata coordinate dimensions");

    std::array<uint64_t, kMaximumTracks> ids{};
    std::array<Box, kMaximumTracks> boxes{};
    size_t count = 0;
    for (const auto* item = frame->obj_meta_list; item; item = item->next) {
      const auto* object = static_cast<const NvDsObjectMeta*>(item->data);
      if (!object || object->class_id != 0 || object->object_id == kUntrackedId)
        continue;
      const Box box{
          object->rect_params.left, object->rect_params.top, object->rect_params.width, object->rect_params.height};
      if (!ValidBox(box) || box.left >= source.width || box.top >= source.height || box.left + box.width <= 0 ||
          box.top + box.height <= 0) {
        ++counters.invalid_rois;
        continue;
      }
      if (std::find(ids.begin(), ids.begin() + count, object->object_id) != ids.begin() + count)
        continue;
      if (count == kMaximumTracks) {
        ++counters.capacity_excluded;
        // A changed metadata list order must not exclude an admitted identity
        // in favor of a new detection when the hard candidate bound is full.
        if (source.scheduler.Find(object->object_id)) {
          for (size_t i = count; i > 0; --i) {
            if (!source.scheduler.Find(ids[i - 1])) {
              ids[i - 1] = object->object_id;
              boxes[i - 1] = box;
              break;
            }
          }
        }
        continue;
      }
      ids[count] = object->object_id;
      boxes[count++] = box;
    }
    const uint64_t excluded_before = source.scheduler.excluded_count();
    if (!source.scheduler.BeginFrame(source.epoch, frame->buf_pts, ids.data(), count)) {
      ++counters.duplicate_time;
      return absl::OkStatus();
    }
    counters.capacity_excluded += source.scheduler.excluded_count() - excluded_before;
    // Admission and incarnation belong to the scheduler. Remove all evidence
    // when an identity expires/reappears, including same-ID admission this frame.
    for (auto it = source.evidence.begin(); it != source.evidence.end();) {
      const auto* record = source.scheduler.Find(it->first);
      if (!record || record->incarnation != it->second.incarnation)
        it = source.evidence.erase(it);
      else
        ++it;
    }
    for (size_t i = 0; i < count; ++i) {
      const auto* record = source.scheduler.Find(ids[i]);
      if (!record || !record->visible)
        continue;
      auto inserted = source.evidence.try_emplace(ids[i]);
      auto& evidence = inserted.first->second;
      if (inserted.second) {
        evidence.incarnation = record->incarnation;
        if (jersey)
          evidence.jersey = std::make_unique<JerseyConsensus>();
        if (action)
          evidence.action = std::make_unique<ActionState>();
      }
    }
    for (auto& [id, evidence] : source.evidence) {
      const auto* record = source.scheduler.Find(id);
      if (evidence.action && evidence.action->last_pose != kInvalidTime &&
          (!record->visible || frame->buf_pts - evidence.action->last_pose > kActionMaximumGap)) {
        evidence.action->Reset();
        ++counters.action_history_resets;
      }
    }

    DueWork due;
    for (size_t feature = 0; feature < 3; ++feature) {
      if (!intervals[feature])
        continue;
      std::array<const TrackRecord*, kMaximumTracks> eligible{};
      size_t eligible_count = 0;
      for (size_t i = 0; i < count; ++i) {
        const auto* record = source.scheduler.Find(ids[i]);
        if (!record || !record->visible)
          continue;
        if (feature == 1 && config.jersey_roi_mode == JerseyRoiMode::kPose && record->last_attempt[1] != kInvalidTime &&
            frame->buf_pts - record->last_attempt[1] < intervals[1])
          continue;
        if (feature == 1 && config.jersey_roi_mode == JerseyRoiMode::kPose && record->last_attempt[0] != kInvalidTime &&
            frame->buf_pts - record->last_attempt[0] < intervals[0]) {
          ++counters.jersey_pose_skipped;
          continue;
        }
        if (feature == 2) {
          const auto& state = *source.evidence.at(ids[i]).action;
          if (!state.history.ready(frame->buf_pts)) {
            ++counters.action_history_unready;
            continue;
          }
          if (state.history.window_end() == state.attempted_window)
            continue;
        }
        if (record->last_attempt[feature] == kInvalidTime ||
            frame->buf_pts - record->last_attempt[feature] >= intervals[feature])
          eligible[eligible_count++] = record;
      }
      // Filter readiness BEFORE truncation so warming tracks cannot starve a
      // ready action track outside the first32 due identities.
      std::sort(eligible.begin(), eligible.begin() + eligible_count, [feature](const auto* a, const auto* b) {
        const auto at = a->last_attempt[feature], bt = b->last_attempt[feature];
        if (at != bt)
          return at == kInvalidTime || (bt != kInvalidTime && at < bt);
        return a->id < b->id;
      });
      due.counts[feature] = eligible_count;
      for (size_t i = 0; i < eligible_count; ++i)
        due.ids[feature][i] = eligible[i]->id;
    }
    const auto plan = PlanFrameWork(
        due,
        config.maximum_due_rois,
        config.jersey.enabled && config.jersey_roi_mode == JerseyRoiMode::kPose,
        &source.scheduling_cursor);
    for (size_t f = 0; f < 3; ++f)
      counters.budget_deferred += due.counts[f] - plan.counts[f];
    std::unique_ptr<BorrowedImage> image;
    if (plan.counts[0] || plan.counts[1]) {
      auto mapped = BorrowedImage::Map(surface, frame->batch_id, gpu_id);
      if (!mapped.ok())
        return mapped.status();
      image = std::move(*mapped);
    }
    // Fence lifetime follows image lifetime on every return and exception.
    StreamFence fence{stream};
    FrameResult result;
    result.stream_id = frame->source_id;
    result.epoch = source.epoch;
    result.sequence = frame->frame_num;
    result.pts_ns = frame->buf_pts;
    result.geometry_token = source.geometry_token;
    result.coordinate_width = source.width;
    result.coordinate_height = source.height;
    std::array<Pose, kMaximumTracks> fresh_poses{};
    std::array<bool, kMaximumTracks> has_pose{};
    std::array<size_t, kMaximumBatch> selected{};
    uint64_t frame_samples = 0;
    const auto submitted = [&](size_t samples) {
      frame_samples += samples;
      counters.model_samples += samples;
      counters.maximum_frame_samples = std::max(counters.maximum_frame_samples, frame_samples);
    };
    for (size_t offset = 0; offset < plan.counts[0];) {
      if (Cancelled(publication))
        return absl::CancelledError("Player analytics batch was flushed");
      size_t batch_count = 0;
      while (offset < plan.counts[0] && batch_count < config.batch_size) {
        const uint64_t id = plan.ids[0][offset++];
        source.scheduler.MarkAttempt(Feature::kPose, id);
        const size_t index = std::find(ids.begin(), ids.begin() + count, id) - ids.begin();
        if (index == count)
          continue;
        auto affine = MakePoseAffine(image->view(), {boxes[index], float(source.width), float(source.height)});
        if (!affine.ok()) {
          ++counters.invalid_rois;
          auto& state = source.evidence.at(id).action;
          if (state) {
            state->Reset();
            ++counters.action_history_resets;
          }
          continue;
        }
        selected[batch_count] = index;
        host_affines[batch_count++] = *affine;
      }
      if (!batch_count)
        continue;
      fence.pending = true;
      PA_RETURN_IF_ERROR(Cuda(cudaMemcpyAsync(
          device_affines, host_affines, batch_count * sizeof(PoseAffine), cudaMemcpyHostToDevice, stream)));
      PA_RETURN_IF_ERROR(Cuda(PreprocessPose(image->view(), device_affines, batch_count, pose->input(), stream)));
      PA_RETURN_IF_ERROR(pose->Enqueue(batch_count, stream));
      ++counters.pose_enqueues;
      submitted(batch_count);
      PA_RETURN_IF_ERROR(
          Cuda(DecodePose(pose->output(0), pose->output(1), device_affines, batch_count, device_poses, stream)));
      PA_RETURN_IF_ERROR(
          Cuda(cudaMemcpyAsync(host_poses, device_poses, batch_count * sizeof(Pose), cudaMemcpyDeviceToHost, stream)));
      PA_RETURN_IF_ERROR(fence.Finish());
      for (size_t i = 0; i < batch_count; ++i) {
        const auto index = selected[i];
        has_pose[index] = true;
        fresh_poses[index] = host_poses[i];
        ++counters.pose_results;
        auto& state = source.evidence.at(ids[index]).action;
        if (state) {
          // The model profile owns its >=8 joints at .3 requirement; drawing's
          // configurable threshold must not alter the action model inputs.
          if (state->history.Observe(frame->buf_pts, host_poses[i], source.width, source.height)) {
            state->last_pose = frame->buf_pts;
          } else {
            state->Reset();
            ++counters.action_history_resets;
          }
        }
      }
    }
    std::array<JerseyCrop, kMaximumBatch> crops{};
    for (size_t offset = 0; offset < plan.counts[1];) {
      if (Cancelled(publication))
        return absl::CancelledError("Player analytics batch was flushed");
      size_t batch_count = 0;
      while (offset < plan.counts[1] && batch_count < config.batch_size) {
        const uint64_t id = plan.ids[1][offset++];
        source.scheduler.MarkAttempt(Feature::kJersey, id);
        const size_t index = std::find(ids.begin(), ids.begin() + count, id) - ids.begin();
        if (index == count)
          continue;
        const auto crop = MakeJerseyCrop(
            image->view(),
            boxes[index],
            source.width,
            source.height,
            config.jersey_roi_mode,
            has_pose[index] ? &fresh_poses[index] : nullptr);
        if (!crop) {
          if (crop.reason == JerseyCropReason::kInsufficientPose)
            ++counters.jersey_pose_skipped;
          else
            ++counters.invalid_rois;
          continue;
        }
        // OCR needs a minimally resolved, mostly visible torso. Padding remains
        // GPU-native; crops with less than half their area in-frame are skipped.
        const auto& c = crop.crop;
        const int visible_width =
            std::max(0, std::min(c.left + c.width, int(image->view().width)) - std::max(c.left, 0));
        const int visible_height =
            std::max(0, std::min(c.top + c.height, int(image->view().height)) - std::max(c.top, 0));
        if (c.width < 16 || c.height < 8 || int64_t(visible_width) * visible_height * 2 < int64_t(c.width) * c.height) {
          ++counters.jersey_visibility_skipped;
          continue;
        }
        selected[batch_count] = index;
        crops[batch_count++] = c;
      }
      if (!batch_count)
        continue;
      fence.pending = true;
      PA_RETURN_IF_ERROR(Cuda(PreprocessJersey(
          image->view(), crops.data(), batch_count, jersey_scratch, jersey_scratch_bytes, jersey->input(), stream)));
      PA_RETURN_IF_ERROR(jersey->Enqueue(batch_count, stream));
      ++counters.jersey_enqueues;
      submitted(batch_count);
      PA_RETURN_IF_ERROR(Cuda(DecodeJersey(jersey->output(0), batch_count, jersey_vocabulary, device_jerseys, stream)));
      PA_RETURN_IF_ERROR(Cuda(cudaMemcpyAsync(
          host_jerseys, device_jerseys, batch_count * sizeof(JerseyObservation), cudaMemcpyDeviceToHost, stream)));
      PA_RETURN_IF_ERROR(fence.Finish());
      for (size_t i = 0; i < batch_count; ++i) {
        const auto& observation = host_jerseys[i];
        source.evidence.at(ids[selected[i]])
            .jersey->Observe(
                frame->buf_pts,
                std::string_view(observation.text, std::min(size_t(observation.length), size_t{2})),
                observation.confidence,
                config.jersey.confidence_threshold);
        ++counters.jersey_results;
      }
    }
    for (size_t offset = 0; offset < plan.counts[2];) {
      if (Cancelled(publication))
        return absl::CancelledError("Player analytics batch was flushed");
      size_t batch_count = 0;
      while (offset < plan.counts[2] && batch_count < config.batch_size) {
        const uint64_t id = plan.ids[2][offset++];
        auto& state = *source.evidence.at(id).action;
        // A planned current pose can invalidate a previously ready history.
        if (state.history.window_end() == state.attempted_window ||
            !state.history.WriteTensor(
                frame->buf_pts,
                host_action_tensor + batch_count * ActionHistory::kTensorFloats,
                ActionHistory::kTensorFloats))
          continue;
        source.scheduler.MarkAttempt(Feature::kAction, id);
        state.attempted_window = state.history.window_end();
        selected[batch_count++] = std::find(ids.begin(), ids.begin() + count, id) - ids.begin();
      }
      if (!batch_count)
        continue;
      fence.pending = true;
      PA_RETURN_IF_ERROR(Cuda(cudaMemcpyAsync(
          action->input(),
          host_action_tensor,
          batch_count * ActionHistory::kTensorFloats * sizeof(float),
          cudaMemcpyHostToDevice,
          stream)));
      PA_RETURN_IF_ERROR(action->Enqueue(batch_count, stream));
      ++counters.action_enqueues;
      submitted(batch_count);
      PA_RETURN_IF_ERROR(Cuda(ReduceAction(action->output(0), batch_count, device_actions, stream)));
      PA_RETURN_IF_ERROR(Cuda(cudaMemcpyAsync(
          host_actions, device_actions, batch_count * sizeof(ActionObservation), cudaMemcpyDeviceToHost, stream)));
      PA_RETURN_IF_ERROR(fence.Finish());
      for (size_t i = 0; i < batch_count; ++i) {
        auto& state = *source.evidence.at(ids[selected[i]]).action;
        state.label.Observe(
            state.history.window_end(),
            state.history.window_start(),
            host_actions[i].label,
            host_actions[i].confidence,
            config.action.confidence_threshold);
        ++counters.action_results;
      }
    }
    for (size_t i = 0; i < count; ++i) {
      const auto found = source.evidence.find(ids[i]);
      if (found == source.evidence.end())
        continue;
      auto& evidence = found->second;
      const auto jersey_label = evidence.jersey ? evidence.jersey->Result(frame->buf_pts) : JerseyResult{};
      const auto action_label = evidence.action && evidence.action->history.ready(frame->buf_pts)
          ? evidence.action->label.Result(frame->buf_pts)
          : ActionResult{};
      if (!has_pose[i] && !jersey_label.text[0] && action_label.label < 0)
        continue;
      auto& player = result.players[result.player_count++];
      player.track_id = ids[i];
      player.box = boxes[i];
      player.jersey = jersey_label;
      player.action = action_label;
      if (has_pose[i]) {
        player.has_pose = true;
        player.pose_observed_at = frame->buf_pts;
        player.pose = fresh_poses[i];
        for (auto& keypoint : player.pose)
          if (keypoint.confidence < config.pose.confidence_threshold)
            keypoint.confidence = 0;
      }
    }
    std::lock_guard<std::mutex> publication_lock(publication_mutex);
    if (Cancelled(publication))
      return absl::CancelledError("Player analytics batch was flushed");
    if (result.player_count && !AttachFrameResult(frame, result))
      return absl::ResourceExhaustedError("Cannot attach immutable player analytics results");
    return absl::OkStatus();
  }
};

PlayerAnalyticsProcessor::PlayerAnalyticsProcessor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PlayerAnalyticsProcessor::~PlayerAnalyticsProcessor() = default;

absl::StatusOr<std::unique_ptr<PlayerAnalyticsProcessor>> PlayerAnalyticsProcessor::Create(
    const Config& config,
    int gpu_id) {
  if (!config.enabled())
    return std::unique_ptr<PlayerAnalyticsProcessor>{};
  if (gpu_id < 0 || config.batch_size == 0 || config.batch_size > kMaximumBatch || config.maximum_tracks == 0 ||
      config.maximum_tracks > kMaximumTracks || config.maximum_due_rois == 0 ||
      config.maximum_due_rois > kMaximumDueRois || config.batch_size > config.maximum_due_rois ||
      config.track_retention_ns < 1000000 || config.track_retention_ns > 60 * kSecond ||
      (config.action.enabled && (!config.pose.enabled || config.pose.rate_hz < 10)) ||
      (config.jersey.enabled && config.jersey_roi_mode != JerseyRoiMode::kBox &&
       config.jersey_roi_mode != JerseyRoiMode::kPose) ||
      (config.jersey.enabled && config.jersey_roi_mode == JerseyRoiMode::kPose &&
       (!config.pose.enabled || config.maximum_due_rois < 2)))
    return absl::InvalidArgumentError("Invalid player analytics execution bounds or pose dependency");
  auto impl = std::make_unique<Impl>();
  impl->config = config;
  impl->gpu_id = gpu_id;
  const std::array<const FeatureConfig*, 3> features{{&config.pose, &config.jersey, &config.action}};
  for (size_t i = 0; i < features.size(); ++i) {
    const auto& feature = *features[i];
    if (!feature.enabled)
      continue;
    if (!std::isfinite(feature.rate_hz) || feature.rate_hz <= 0 || feature.rate_hz > 240 ||
        !std::isfinite(feature.confidence_threshold) || feature.confidence_threshold < 0 ||
        feature.confidence_threshold > 1 || feature.bundle.empty() || feature.bundle.size() > 4096 ||
        feature.bundle.find('\0') != std::string::npos)
      return absl::InvalidArgumentError("Invalid enabled player analytics model settings");
    const double interval = std::ceil(double(kSecond) / feature.rate_hz);
    if (!std::isfinite(interval) || interval >= double(kInvalidTime))
      return absl::InvalidArgumentError("Player analytics cadence exceeds source-time range");
    impl->intervals[i] = static_cast<uint64_t>(interval);
  }
  PA_RETURN_IF_ERROR(Cuda(cudaSetDevice(gpu_id)));
  PA_RETURN_IF_ERROR(Cuda(cudaStreamCreateWithFlags(&impl->stream, cudaStreamNonBlocking)));
  if (config.pose.enabled) {
    auto engine = NativeEngine::Load(config.pose.bundle, ModelFeature::kPose, gpu_id, config.batch_size);
    if (!engine.ok())
      return engine.status();
    impl->pose = std::move(*engine);
    PA_RETURN_IF_ERROR(
        Cuda(cudaMalloc(reinterpret_cast<void**>(&impl->device_affines), config.batch_size * sizeof(PoseAffine))));
    PA_RETURN_IF_ERROR(
        Cuda(cudaMalloc(reinterpret_cast<void**>(&impl->device_poses), config.batch_size * sizeof(Pose))));
    PA_RETURN_IF_ERROR(
        Cuda(cudaMallocHost(reinterpret_cast<void**>(&impl->host_affines), config.batch_size * sizeof(PoseAffine))));
    PA_RETURN_IF_ERROR(
        Cuda(cudaMallocHost(reinterpret_cast<void**>(&impl->host_poses), config.batch_size * sizeof(Pose))));
  }
  if (config.jersey.enabled) {
    auto engine = NativeEngine::Load(config.jersey.bundle, ModelFeature::kJersey, gpu_id, config.batch_size);
    if (!engine.ok())
      return engine.status();
    auto vocabulary = MakeJerseyVocabulary((*engine)->manifest());
    if (!vocabulary.ok())
      return vocabulary.status();
    impl->jersey_vocabulary = *vocabulary;
    impl->jersey = std::move(*engine);
    impl->jersey_scratch_bytes = JerseyScratchBytes(config.batch_size);
    PA_RETURN_IF_ERROR(Cuda(cudaMalloc(&impl->jersey_scratch, impl->jersey_scratch_bytes)));
    PA_RETURN_IF_ERROR(Cuda(
        cudaMalloc(reinterpret_cast<void**>(&impl->device_jerseys), config.batch_size * sizeof(JerseyObservation))));
    PA_RETURN_IF_ERROR(Cuda(
        cudaMallocHost(reinterpret_cast<void**>(&impl->host_jerseys), config.batch_size * sizeof(JerseyObservation))));
  }
  if (config.action.enabled) {
    auto engine = NativeEngine::Load(config.action.bundle, ModelFeature::kAction, gpu_id, config.batch_size);
    if (!engine.ok())
      return engine.status();
    impl->action = std::move(*engine);
    PA_RETURN_IF_ERROR(Cuda(
        cudaMalloc(reinterpret_cast<void**>(&impl->device_actions), config.batch_size * sizeof(ActionObservation))));
    PA_RETURN_IF_ERROR(Cuda(
        cudaMallocHost(reinterpret_cast<void**>(&impl->host_actions), config.batch_size * sizeof(ActionObservation))));
    PA_RETURN_IF_ERROR(Cuda(cudaMallocHost(
        reinterpret_cast<void**>(&impl->host_action_tensor),
        config.batch_size * ActionHistory::kTensorFloats * sizeof(float))));
  }
  return std::unique_ptr<PlayerAnalyticsProcessor>(new PlayerAnalyticsProcessor(std::move(impl)));
}

absl::Status PlayerAnalyticsProcessor::Process(NvBufSurface* surface, NvDsBatchMeta* batch) {
  const uint64_t publication = impl_->publication_epoch.load(std::memory_order_acquire);
  if (impl_->Cancelled(publication)) {
    ++impl_->counters.cancelled_batches;
    return absl::CancelledError("Player analytics is flushing");
  }
  if (!batch || !surface || !surface->surfaceList || surface->numFilled == 0 ||
      surface->numFilled > surface->batchSize || surface->batchSize > 64 ||
      batch->num_frames_in_batch > surface->numFilled)
    return absl::InvalidArgumentError("Player analytics requires DeepStream surface and batch metadata");
  PA_RETURN_IF_ERROR(Cuda(cudaSetDevice(impl_->gpu_id)));
  uint64_t seen_surfaces = 0;
  unsigned frames = 0;
  for (auto* item = batch->frame_meta_list; item; item = item->next) {
    auto* frame = static_cast<NvDsFrameMeta*>(item->data);
    if (!frame || ++frames > 64 || frame->batch_id >= surface->numFilled ||
        (seen_surfaces & (uint64_t{1} << frame->batch_id)))
      return absl::InvalidArgumentError("Invalid or duplicate player analytics frame surface index");
    seen_surfaces |= uint64_t{1} << frame->batch_id;
    const auto status = impl_->Frame(surface, frame, publication);
    if (!status.ok()) {
      if (absl::IsCancelled(status))
        ++impl_->counters.cancelled_batches;
      return status;
    }
  }
  if (impl_->Cancelled(publication)) {
    ++impl_->counters.cancelled_batches;
    return absl::CancelledError("Player analytics batch was flushed");
  }
  if (frames != batch->num_frames_in_batch)
    return absl::InvalidArgumentError("Player analytics batch frame count disagrees with metadata");
  return absl::OkStatus();
}

void PlayerAnalyticsProcessor::CancelPending() noexcept {
  std::lock_guard<std::mutex> lock(impl_->publication_mutex);
  impl_->flushing.store(true, std::memory_order_release);
  impl_->publication_epoch.fetch_add(1, std::memory_order_acq_rel);
}
void PlayerAnalyticsProcessor::Reset(bool finish_flush) noexcept {
  impl_->sources.clear();
  std::lock_guard<std::mutex> lock(impl_->publication_mutex);
  impl_->publication_epoch.fetch_add(1, std::memory_order_acq_rel);
  if (finish_flush)
    impl_->flushing.store(false, std::memory_order_release);
}
const AnalyticsCounters& PlayerAnalyticsProcessor::counters() const noexcept {
  return impl_->counters;
}

} // namespace hm::player_analytics
