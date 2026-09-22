#include "Int8FrameSampler.h"

#include <cuda_runtime.h>
#include <gstnvdsmeta.h>
#include <nvbufsurftransform.h>
#include <opencv2/opencv.hpp>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "absl/cleanup/cleanup.h"

#include "Int8SamplePlan.h"
#include "deepstream_app.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/StitchingFramePairMeta.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

namespace hm::pipeline {
namespace {
absl::Status error(const std::string& text) {
  return absl::FailedPreconditionError("INT8 sampling: " + text);
}
} // namespace

absl::StatusOr<std::unique_ptr<Int8FrameSampler>> Int8FrameSampler::Create(
    AppCtx* app,
    const std::filesystem::path& game,
    const std::filesystem::path& output,
    uint64_t duration_ns,
    size_t count,
    int width,
    int height) {
  auto result = std::unique_ptr<Int8FrameSampler>(new Int8FrameSampler);
  HM_ASSIGN_OR_RETURN(result->times_, Int8SampleTimes(duration_ns, count));
  if (!app || !app->pipeline.multi_src_bin.uri_playlist_exact_pairing_enabled ||
      !app->pipeline.hmstitcher_bin.elem_hmstitcher || width < 16 || height < 16 || width > 4096 || height > 4096 ||
      static_cast<int64_t>(width) * height > 4194304)
    return error("requires two recorded cameras, completed stitching and at most 4 megapixel detector input");
  if (std::filesystem::exists(output))
    return error("sample output must be a new directory");
  std::filesystem::create_directories(output);
  result->app_ = app;
  result->game_ = game;
  result->output_ = std::filesystem::absolute(output);
  result->width_ = width;
  result->height_ = height;
  HM_ASSIGN_OR_RETURN(result->generation_, hm::stitching::current_stitched_output_generation_id(game));
  result->manifest_["schema"] = 1;
  result->manifest_["game"] = std::filesystem::canonical(game).string();
  result->manifest_["generation"] = result->generation_;
  result->manifest_["duration_ns"] = duration_ns;
  result->manifest_["width"] = width;
  result->manifest_["height"] = height;
  for (guint camera = 0; camera < app->pipeline.multi_src_bin.num_bins; ++camera) {
    const auto& source = app->pipeline.multi_src_bin.sub_bins[camera];
    for (guint chapter = 0; chapter < source.num_uri_list; ++chapter) {
      gchar* path = g_filename_from_uri(source.uri_list[chapter], nullptr, nullptr);
      if (!path)
        return error("sampling requires local recorded files");
      auto binding = hm::stitching::BindPlayerFrameSource(path);
      g_free(path);
      if (!binding.ok())
        return binding.status();
      YAML::Node item;
      item["path"] = binding->path;
      item["size"] = binding->size;
      item["mtime_ns"] = binding->modification_time_ns;
      result->manifest_["sources"].push_back(item);
    }
  }
  result->pad_ = gst_element_get_static_pad(app->pipeline.hmstitcher_bin.elem_hmstitcher, "src");
  if (!result->pad_)
    return error("stitcher output is unavailable");
  result->probe_ = gst_pad_add_probe(result->pad_, GST_PAD_PROBE_TYPE_BUFFER, Observe, result.get(), nullptr);
  if (!result->probe_)
    return error("could not attach capture observer");
  return result;
}

Int8FrameSampler::~Int8FrameSampler() {
  if (pad_) {
    if (probe_)
      gst_pad_remove_probe(pad_, probe_);
    gst_object_unref(pad_);
  }
}

GstPadProbeReturn Int8FrameSampler::Observe(GstPad*, GstPadProbeInfo* info, gpointer data) {
  auto* self = static_cast<Int8FrameSampler*>(data);
  try {
    if (self->failure_.ok())
      self->failure_ = self->Sample(GST_PAD_PROBE_INFO_BUFFER(info));
  } catch (const std::exception& exception) {
    self->failure_ = error(exception.what());
  }
  if (!self->failure_.ok()) {
    GError* cause = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, self->failure_.ToString().c_str());
    gst_element_post_message(
        self->app_->pipeline.pipeline,
        gst_message_new_error(GST_OBJECT(self->app_->pipeline.pipeline), cause, "INT8 sample capture failed"));
    g_error_free(cause);
    return GST_PAD_PROBE_DROP;
  }
  return GST_PAD_PROBE_OK;
}

absl::Status Int8FrameSampler::Sample(GstBuffer* buffer) {
  if (complete_)
    return absl::OkStatus();
  auto* batch = gst_buffer_get_nvds_batch_meta(buffer);
  if (!batch || batch->num_frames_in_batch != 1 || !batch->frame_meta_list)
    return error("expected one stitched output frame");
  auto* frame = static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data);
  const auto pair = hm::stitching_frame_pair(frame);
  if (!pair || !GST_CLOCK_TIME_IS_VALID(pair->timeline_pts))
    return error("missing synchronized frame-pair identity");
  if (pair->timeline_pts < times_.at(next_))
    return absl::OkStatus();
  const auto* generation = hm::stitching::find_stitched_output_generation_meta(frame);
  if (!generation || generation->generation() != generation_)
    return error("stitching geometry changed during capture");
  GstMapInfo map = GST_MAP_INFO_INIT;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
    return error("could not access NVMM surface descriptor");
  auto unmap = absl::MakeCleanup([&] { gst_buffer_unmap(buffer, &map); });
  auto* input = reinterpret_cast<NvBufSurface*>(map.data);
  if (!input || frame->batch_id >= input->numFilled)
    return error("invalid NVMM batch");
  auto& source = input->surfaceList[frame->batch_id];
  manifest_["gpu_id"] = input->gpuId;
  // Match gst-nvinfer get_converted_buffer: even source ROI, truncated
  // aspect-preserving destination and symmetric black padding. Save the final
  // network-sized image, so the offline quantizer need not resize it again.
  const guint source_width = GST_ROUND_DOWN_2(source.width);
  const guint source_height = GST_ROUND_DOWN_2(source.height);
  if (!source_width || !source_height)
    return error("empty stitched canvas");
  const double destination_height = static_cast<double>(width_) * source_height / source_width;
  const guint resized_width = destination_height <= height_
      ? width_
      : static_cast<guint>(static_cast<double>(height_) * source_width / source_height);
  const guint resized_height = destination_height <= height_ ? static_cast<guint>(destination_height) : height_;
  if (!resized_width || !resized_height)
    return error("stitched aspect ratio is too extreme for detector input");
  NvBufSurfaceCreateParams create{};
  create.gpuId = input->gpuId;
  create.width = width_;
  create.height = height_;
  create.layout = NVBUF_LAYOUT_PITCH;
  create.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  create.memType = NVBUF_MEM_CUDA_DEVICE;
  NvBufSurface* resized = nullptr;
  if (cudaSetDevice(input->gpuId) != cudaSuccess || NvBufSurfaceCreate(&resized, 1, &create) != 0)
    return error("could not allocate bounded GPU capture surface");
  auto release = absl::MakeCleanup([&] { NvBufSurfaceDestroy(resized); });
  auto& surface = resized->surfaceList[0];
  if (cudaMemset2D(surface.dataPtr, surface.pitch, 0, width_ * 4, height_) != cudaSuccess)
    return error("could not clear GPU padding");
  NvBufSurface single = *input;
  single.batchSize = single.numFilled = 1;
  single.surfaceList = &source;
  NvBufSurfTransformConfigParams session{};
  session.compute_mode = NvBufSurfTransformCompute_GPU;
  session.gpu_id = input->gpuId;
  NvBufSurfTransformParams transform{};
  NvBufSurfTransformRect src_rect{0, 0, source_width, source_height};
  NvBufSurfTransformRect dst_rect{
      (height_ - resized_height) / 2, (width_ - resized_width) / 2, resized_width, resized_height};
  transform.src_rect = &src_rect;
  transform.dst_rect = &dst_rect;
  transform.transform_flag = NVBUFSURF_TRANSFORM_FILTER | NVBUFSURF_TRANSFORM_CROP_SRC | NVBUFSURF_TRANSFORM_CROP_DST;
  transform.transform_filter = NvBufSurfTransformInter_Default;
  if (NvBufSurfTransformSetSessionParams(&session) != NvBufSurfTransformError_Success ||
      NvBufSurfTransform(&single, resized, &transform) != NvBufSurfTransformError_Success)
    return error("GPU resize/color conversion failed");
  cv::Mat rgba(height_, width_, CV_8UC4);
  // Offline calibration needs host pixels for ORT quantization. Only selected
  // frames cross this boundary, after GPU resize to at most detector resolution.
  if (cudaMemcpy2D(rgba.data, rgba.step, surface.dataPtr, surface.pitch, width_ * 4, height_, cudaMemcpyDeviceToHost) !=
      cudaSuccess)
    return error("bounded image readback failed");
  cv::Mat bgr;
  cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);
  std::ostringstream name;
  name << "frame-" << std::setw(4) << std::setfill('0') << next_ << ".png";
  if (!cv::imwrite((output_ / name.str()).string(), bgr))
    return error("could not save calibration image");
  YAML::Node sample;
  sample["image"] = name.str();
  sample["target_ns"] = times_[next_];
  sample["timeline_ns"] = pair->timeline_pts;
  sample["canvas_width"] = source.width;
  sample["canvas_height"] = source.height;
  for (const auto& camera : {pair->left, pair->right}) {
    YAML::Node identity;
    identity["uri"] = g_quark_to_string(camera.source_uri);
    identity["source_pts_ns"] = camera.source_pts;
    identity["sequence"] = camera.sequence;
    sample["cameras"].push_back(identity);
  }
  manifest_["samples"].push_back(sample);
  ++next_;
  g_print(
      "HSTREAM_INT8_SAMPLE completed=%zu total=%zu timeline_ns=%" G_GUINT64_FORMAT "\n",
      next_,
      times_.size(),
      pair->timeline_pts);
  complete_ = next_ == times_.size();
  return absl::OkStatus();
}

absl::Status Int8FrameSampler::Verify(const std::filesystem::path& directory) {
  try {
    const auto manifest = YAML::LoadFile((directory / "samples.json").string());
    std::string current;
    HM_ASSIGN_OR_RETURN(
        current, hm::stitching::current_stitched_output_generation_id(manifest["game"].as<std::string>()));
    if (current != manifest["generation"].as<std::string>())
      return error("stitching geometry changed after capture");
    for (const auto& source : manifest["sources"]) {
      auto binding = hm::stitching::BindPlayerFrameSource(source["path"].as<std::string>());
      if (!binding.ok())
        return binding.status();
      if (binding->size != source["size"].as<uint64_t>() ||
          binding->modification_time_ns != source["mtime_ns"].as<int64_t>())
        return error("recorded source changed after capture");
    }
    return absl::OkStatus();
  } catch (const std::exception& exception) {
    return error(exception.what());
  }
}

absl::Status Int8FrameSampler::Finish() {
  HM_RETURN_IF_ERROR(failure_);
  if (!complete_)
    return error("recording ended or capture was cancelled before all samples completed");
  std::string current;
  HM_ASSIGN_OR_RETURN(current, hm::stitching::current_stitched_output_generation_id(game_));
  if (current != generation_)
    return error("stitching geometry changed before publication");
  std::ofstream list(output_ / "images.txt");
  for (const auto& sample : manifest_["samples"])
    list << sample["image"].as<std::string>() << '\n';
  list.close();
  YAML::Emitter emitter;
  emitter << YAML::Flow << YAML::DoubleQuoted << manifest_;
  std::ofstream report(output_ / "samples.json.tmp");
  report << emitter.c_str() << '\n';
  report.close();
  if (!list || !report)
    return error("could not publish complete sample manifest");
  std::filesystem::rename(output_ / "samples.json.tmp", output_ / "samples.json");
  return Verify(output_);
}
} // namespace hm::pipeline
