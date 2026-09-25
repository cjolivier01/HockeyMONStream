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
#include "hstream/src/libs/player_analytics/FrameMeta.h"
#include "hstream/src/libs/player_analytics/TemporalState.h"
#include "hstream/src/libs/player_analytics/runtime/NativeEngine.h"
#include "hstream/src/libs/player_analytics/runtime/PoseGpu.h"
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
  struct Source {
    explicit Source(const Config& config) : scheduler(config.maximum_tracks, config.track_retention_ns) {}
    TrackScheduler scheduler;
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
  PoseAffine* device_affines{nullptr};
  Pose* device_poses{nullptr};
  PoseAffine* host_affines{nullptr};
  Pose* host_poses{nullptr};
  std::map<uint32_t, Source> sources;
  AnalyticsCounters counters;
  uint64_t pose_interval{0};
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
      source.scheduler.Reset();
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
      source.scheduler.Reset();
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
    std::array<uint64_t, kMaximumDueRois> due{};
    const size_t due_count = source.scheduler.SelectDue(Feature::kPose, pose_interval, &due, config.maximum_due_rois);
    if (!due_count)
      return absl::OkStatus();

    auto mapped = BorrowedImage::Map(surface, frame->batch_id, gpu_id);
    if (!mapped.ok())
      return mapped.status();
    auto image = std::move(*mapped);
    StreamFence fence{stream};
    FrameResult result;
    result.stream_id = frame->source_id;
    result.epoch = source.epoch;
    result.sequence = frame->frame_num;
    result.pts_ns = frame->buf_pts;
    result.geometry_token = source.geometry_token;
    result.coordinate_width = source.width;
    result.coordinate_height = source.height;
    std::array<size_t, kMaximumBatch> selected{};
    for (size_t offset = 0; offset < due_count;) {
      if (Cancelled(publication))
        return absl::CancelledError("Player analytics batch was flushed");
      size_t batch_count = 0;
      while (offset < due_count && batch_count < config.batch_size) {
        const uint64_t id = due[offset++];
        source.scheduler.MarkAttempt(Feature::kPose, id);
        const auto found = std::find(ids.begin(), ids.begin() + count, id);
        if (found == ids.begin() + count)
          continue;
        const size_t index = found - ids.begin();
        auto affine = MakePoseAffine(image->view(), {boxes[index], float(source.width), float(source.height)});
        if (!affine.ok()) {
          ++counters.invalid_rois;
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
      PA_RETURN_IF_ERROR(
          Cuda(DecodePose(pose->output(0), pose->output(1), device_affines, batch_count, device_poses, stream)));
      PA_RETURN_IF_ERROR(
          Cuda(cudaMemcpyAsync(host_poses, device_poses, batch_count * sizeof(Pose), cudaMemcpyDeviceToHost, stream)));
      PA_RETURN_IF_ERROR(fence.Finish());
      for (size_t i = 0; i < batch_count; ++i) {
        auto& player = result.players[result.player_count++];
        player.track_id = ids[selected[i]];
        player.box = boxes[selected[i]];
        player.has_pose = true;
        player.pose_observed_at = frame->buf_pts;
        player.pose = host_poses[i];
        for (auto& keypoint : player.pose)
          if (keypoint.confidence < config.pose.confidence_threshold)
            keypoint.confidence = 0;
        ++counters.pose_results;
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
  if (config.jersey.enabled || config.action.enabled)
    return absl::UnimplementedError(
        "This stack stage supports pose inference; "
        "jersey/action runtime arrives next");
  if (gpu_id < 0 || config.batch_size == 0 || config.batch_size > kMaximumBatch || config.maximum_tracks == 0 ||
      config.maximum_tracks > kMaximumTracks || config.maximum_due_rois == 0 ||
      config.maximum_due_rois > kMaximumDueRois || config.batch_size > config.maximum_due_rois ||
      config.track_retention_ns < 1000000 || config.track_retention_ns > 60 * kSecond ||
      !std::isfinite(config.pose.rate_hz) || config.pose.rate_hz <= 0 || config.pose.rate_hz > 240 ||
      !std::isfinite(config.pose.confidence_threshold) || config.pose.confidence_threshold < 0 ||
      config.pose.confidence_threshold > 1 || config.pose.bundle.empty() || config.pose.bundle.size() > 4096 ||
      config.pose.bundle.find('\0') != std::string::npos)
    return absl::InvalidArgumentError("Invalid player analytics execution bounds");
  const double interval = std::ceil(double(kSecond) / config.pose.rate_hz);
  if (!std::isfinite(interval) || interval >= double(kInvalidTime))
    return absl::InvalidArgumentError("Player analytics cadence exceeds source-time range");
  auto impl = std::make_unique<Impl>();
  impl->config = config;
  impl->gpu_id = gpu_id;
  impl->pose_interval = static_cast<uint64_t>(interval);
  PA_RETURN_IF_ERROR(Cuda(cudaSetDevice(gpu_id)));
  PA_RETURN_IF_ERROR(Cuda(cudaStreamCreateWithFlags(&impl->stream, cudaStreamNonBlocking)));
  auto engine = NativeEngine::Load(config.pose.bundle, ModelFeature::kPose, gpu_id, config.batch_size);
  if (!engine.ok())
    return engine.status();
  impl->pose = std::move(*engine);
  PA_RETURN_IF_ERROR(
      Cuda(cudaMalloc(reinterpret_cast<void**>(&impl->device_affines), config.batch_size * sizeof(PoseAffine))));
  PA_RETURN_IF_ERROR(Cuda(cudaMalloc(reinterpret_cast<void**>(&impl->device_poses), config.batch_size * sizeof(Pose))));
  PA_RETURN_IF_ERROR(
      Cuda(cudaMallocHost(reinterpret_cast<void**>(&impl->host_affines), config.batch_size * sizeof(PoseAffine))));
  PA_RETURN_IF_ERROR(
      Cuda(cudaMallocHost(reinterpret_cast<void**>(&impl->host_poses), config.batch_size * sizeof(Pose))));
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
