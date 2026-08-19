#include "hstream/src/gst-plugins/gst-videoprep/playcropper/playcropper.h"
#include <assert.h>
#include <cuda.h>
#include <cuda_runtime.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <npp.h>
#include <opencv4/opencv2/core/types.hpp>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <tuple>
#include <vector>
#include "absl/status/status.h"
#include "absl/strings/str_split.h"
#include "cupano/pano/cudaMat.h"
#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/preputils.h"
#include "hstream/src/gst-plugins/gst-videoprep/playcropper/cudaPlayCropper.h"
#include "hstream/src/gst-plugins/gst-videoprep/playtracker/playtracker_payload.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/draw_display/DrawDisplayMeta.h"
#include "hstream/src/libs/draw_display/Fonts.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "nvdsmeta.h"
#include "yaml-cpp/yaml.h"

#if defined(__aarch64__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "cudaEGL.h"
#endif

// #define SCRATCH_USE_ALIGNED_PITCH

#define PLAYCROPPER_USE_ONE_KERNEL

namespace hm {
namespace playcropper {

// using PlayTrackerPayload = hm::playtracker::PlayTrackerPayload;

namespace {

bool parse_finite_float(const std::string& value, float* out) {
  if (!out || value.empty()) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const float parsed = std::strtof(value.c_str(), &end);
  if (value.c_str() == end || errno == ERANGE || !std::isfinite(parsed)) {
    return false;
  }
  while (end && *end && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (!end || *end != '\0') {
    return false;
  }
  *out = parsed;
  return true;
}

size_t round_down_even(size_t value) {
  if (value <= 2) {
    return 2;
  }
  return (value / 2) * 2;
}

static BBox make_null_tracking_box(const NvBufSurfaceParams* in_surf, const NvBufSurfaceParams* out_surf) {
  BBox surf(0, 0, in_surf->width, in_surf->height);
  double output_ar = double(out_surf->width) / out_surf->height;
  double new_h = in_surf->height;
  double new_w = in_surf->height * output_ar;
  if (new_w > in_surf->width) {
    new_w = in_surf->width;
    new_h = double(in_surf->width) / output_ar;
  }
  assert(new_w <= in_surf->width);
  assert(new_h <= in_surf->height);
  return BBox(0, 0, new_w, new_h).at_center(surf.center());
}

Point rotate_point(const Point& point, const Point& anchor_point, float angle_degrees) {
  const float radians = angle_degrees * (M_PI / 180.0f);
  const float sin_theta = std::sin(radians);
  const float cos_theta = std::cos(radians);
  const float dx = point.x - anchor_point.x;
  const float dy = point.y - anchor_point.y;
  return Point{
      .x = anchor_point.x + (dx * cos_theta - dy * sin_theta), .y = anchor_point.y + (dx * sin_theta + dy * cos_theta)};
}

Point input_point_to_output(
    const Point& input_point,
    const BBox& src_rect,
    float angle,
    const Point& anchor_point,
    const BBox& crop_box,
    const BBox& output_rect) {
  const Point cropped_point{.x = input_point.x - src_rect.left, .y = input_point.y - src_rect.top};
  const Point crop_space_point = rotate_point(cropped_point, anchor_point, angle);
  return Point{
      .x = output_rect.left + ((crop_space_point.x - crop_box.left) / crop_box.width()) * output_rect.width(),
      .y = output_rect.top + ((crop_space_point.y - crop_box.top) / crop_box.height()) * output_rect.height()};
}

bool transform_box_to_output(
    const BBox& source_box,
    float scale_w,
    float scale_h,
    const BBox& src_rect,
    float angle,
    const Point& anchor_point,
    const BBox& crop_box,
    const BBox& output_rect,
    BBox* transformed_box) {
  const BBox input_box = source_box.make_canvas_scaled(scale_w, scale_h);
  const std::array<Point, 4> points = {
      Point{.x = input_box.left, .y = input_box.top},
      Point{.x = input_box.right, .y = input_box.top},
      Point{.x = input_box.right, .y = input_box.bottom},
      Point{.x = input_box.left, .y = input_box.bottom},
  };

  float left = std::numeric_limits<float>::max();
  float top = std::numeric_limits<float>::max();
  float right = std::numeric_limits<float>::lowest();
  float bottom = std::numeric_limits<float>::lowest();
  for (const Point& point : points) {
    const Point output_point = input_point_to_output(point, src_rect, angle, anchor_point, crop_box, output_rect);
    left = std::min(left, output_point.x);
    top = std::min(top, output_point.y);
    right = std::max(right, output_point.x);
    bottom = std::max(bottom, output_point.y);
  }

  transformed_box->left = std::max(left, output_rect.left);
  transformed_box->top = std::max(top, output_rect.top);
  transformed_box->right = std::min(right, output_rect.right);
  transformed_box->bottom = std::min(bottom, output_rect.bottom);
  return transformed_box->right > transformed_box->left && transformed_box->bottom > transformed_box->top;
}

void set_bbox_coords(NvBbox_Coords& coords, const BBox& box) {
  coords.left = box.left;
  coords.top = box.top;
  coords.width = box.width();
  coords.height = box.height();
}

void clear_bbox_coords(NvBbox_Coords& coords) {
  coords.left = 0;
  coords.top = 0;
  coords.width = 0;
  coords.height = 0;
}

bool bbox_coords_are_valid(const NvBbox_Coords& coords) {
  return coords.width > 0 && coords.height > 0;
}

} // namespace

FrameTransformGeometry CalculateFrameTransformGeometry(
    size_t input_width,
    size_t input_height,
    const BBox& tracking_box,
    bool no_crop,
    float fixed_edge_rotation_angle_left,
    float fixed_edge_rotation_angle_right) {
  const BBox input_rect(0, 0, input_width, input_height);
  const float half_width = static_cast<float>(input_width) / 2.0f;
  const float tracking_center_x = tracking_box.center().x;
  float angle = 0.0f;
  if (half_width > 0.0f && tracking_center_x < half_width) {
    const float pct = 1.0f - tracking_center_x / half_width;
    angle = fixed_edge_rotation_angle_left * pct;
  } else if (half_width > 0.0f && tracking_center_x > half_width) {
    const float pct = (half_width - tracking_center_x) / half_width;
    angle = fixed_edge_rotation_angle_right * pct;
  }

  if (no_crop) {
    // HockeyMOM applies perspective rotation before its final no-crop frame
    // transform. Keep the camera box as the rotation anchor while selecting
    // the full rotated input for output.
    return FrameTransformGeometry{input_rect, tracking_box.center(), input_rect, angle};
  }

  const FloatValue minimum_width_per_side = tracking_box.width() / 2;
  const FloatValue clip_left = std::max(input_rect.left, tracking_center_x - minimum_width_per_side);
  const FloatValue clip_right = std::min(input_rect.right, tracking_center_x + minimum_width_per_side);
  const BBox source_rect(clip_left, input_rect.top, clip_right, input_rect.bottom);
  BBox local_tracking_box = tracking_box;
  local_tracking_box.left -= source_rect.left;
  local_tracking_box.right -= source_rect.left;
  return FrameTransformGeometry{source_rect, local_tracking_box.center(), local_tracking_box, angle};
}

absl::Status PlayCropperPriv::PreCapsInit(DSCustom_CreateParams* params) {
  // Not an in-place transform
  m_inVideoFmt = GST_VIDEO_FORMAT_RGBA;
  m_outVideoFmt = GST_VIDEO_FORMAT_RGBA;
  if (params && params->config_file) {
    config_file_ = params->config_file;
  }
  guint output_width = 0;
  guint output_height = 0;
  g_object_get(G_OBJECT(params->m_element), "output-width", &output_width, "output-height", &output_height, NULL);
  runtime_output_size_ = !(output_width && output_height);
  return Super::PreCapsInit(params);
};

absl::Status PlayCropperPriv::PostCapsInit(DSCustom_CreateParams* params) {
  m_transformMode = true;

  return Super::PostCapsInit(params);
}

bool PlayCropperPriv::UsesRuntimeOutputSize() const {
  return runtime_output_size_;
}

absl::StatusOr<videoprep::RuntimeOutputSize> PlayCropperPriv::PrepareRuntimeOutputSize(
    NvDsBatchMeta* /*batch_meta*/,
    NvBufSurface* in_surface) {
  if (!in_surface || !in_surface->surfaceList || !in_surface->batchSize || !in_surface->numFilled) {
    return absl::InvalidArgumentError("Cannot determine playcropper output size without input surface");
  }
  if (in_surface->numFilled > in_surface->batchSize) {
    return absl::InvalidArgumentError("Playcropper input contains more filled surfaces than its batch capacity");
  }

  constexpr double kOutputAspectRatio = 16.0 / 9.0;
  const size_t input_width = in_surface->surfaceList[0].width;
  const size_t input_height = in_surface->surfaceList[0].height;
  size_t output_height = round_down_even(input_height);
  size_t output_width = no_crop_ ? round_down_even(input_width)
                                 : round_down_even(static_cast<size_t>(kOutputAspectRatio * output_height));

  if (runtime_output_max_width_ && runtime_output_max_height_) {
    std::tie(output_width, output_height) =
        resize_to_fit(output_width, output_height, runtime_output_max_width_, runtime_output_max_height_);
    output_width = round_down_even(output_width);
    output_height = round_down_even(output_height);
  }

  // Size the persistent pool from allocation capacity, not this buffer's occupancy. numFilled can be smaller than a
  // later buffer without requiring caps renegotiation or pool growth.
  return videoprep::RuntimeOutputSize{output_width, output_height, in_surface->batchSize};
}

gint PlayCropperPriv::AllocateScratchBuffers(videoprep::GstVideoPrep* videoprep) {
#ifndef PLAYCROPPER_USE_ONE_KERNEL
  cudaError_t cudaErr;

  assert(videoprep->input_width && videoprep->input_height);

  cudaErr = cudaSetDevice(m_gpuId);
  if (cudaErr != cudaSuccess) {
    printf("\n *** Unable to set device in %s Line %d\n", __func__, __LINE__);
    return cudaErr;
  }

#if 1
  static bool ranthis = false;
  (void)ranthis;
  assert(!ranthis);
  ranthis = true;

  hm::WHDims src_size{.width = (FloatValue)videoprep->input_width, .height = (FloatValue)videoprep->input_height};
  constexpr FloatValue out_ar = 16.0 / 9.0;
  FloatValue virt_out_width = ((FloatValue)videoprep->input_height) * out_ar;
  hm::WHDims output_size{.width = virt_out_width, .height = (FloatValue)videoprep->input_height};

  videoprep->pre_rotate_size = get_box_size_necessary_for_rotations(src_size, output_size);

#endif

  constexpr size_t kBytesPerPixel = 4;
#ifdef SCRATCH_USE_ALIGNED_PITCH
  size_t pitch = NVBUF_PLATFORM_ALIGNED_PITCH((size_t)videoprep->pre_rotate_size.width * kBytesPerPixel);
#else
  // No alignment for simpler functions that cant handle aligned pitch
  size_t pitch = (size_t)videoprep->pre_rotate_size.width * kBytesPerPixel;
#endif
  constexpr size_t kNumScratchBuffers = 2;
  for (size_t i = 0; i < kNumScratchBuffers; ++i) {
    void* surface_ptr = nullptr;
    cuda_ck(cudaMalloc(&surface_ptr, pitch * (size_t)videoprep->pre_rotate_size.height));
    // cuda_ck(cudaMallocHost(&surface_ptr, pitch * (size_t)videoprep->pre_rotate_size.height));

    videoprep->priv->scratch_buffers.add_surface(
        surface_ptr,
        videoprep->pre_rotate_size.width,
        videoprep->pre_rotate_size.height,
        pitch,
        kBytesPerPixel,
        /*owns=*/true);
  }
#endif
  return 0;
}

bool PlayCropperPriv::SetProperty(const Property& prop) {
  absl::WriterMutexLock lk(&mu_process_);
  // std::cerr << "SetProperty(" << prop.key << "=" << prop.value << ")" << std::endl;
  std::string key = prop.key;
  std::replace(key.begin(), key.end(), '_', '-');
  if (key == "show") {
    show_ = !!std::atol(prop.value.c_str());
  } else if (key == "render-scale") {
    render_scale_ = std::atof(prop.value.c_str());
    if (render_scale_ == 0) {
      std::cerr << "Invalid render scale: " << render_scale_ << std::endl;
      return false;
    }
  } else if (key == "scoreboard-perspective-polygon") {
    scoreboard_perspective_polygion_.clear();
    std::vector<std::string> points = absl::StrSplit(prop.value, ',');
    assert(points.size() == 8);
    for (size_t i = 0, n = points.size() >> 1; i < n; ++i) {
      const size_t index = i << 1;
      scoreboard_perspective_polygion_.emplace_back(
          cv::Point2f(std::atof(points[index].c_str()), std::atof(points.at(index + 1).c_str())));
    }
    assert(scoreboard_perspective_polygion_.size() == 4);
    scoreboard_disabled_ = std::all_of(
        scoreboard_perspective_polygion_.begin(), scoreboard_perspective_polygion_.end(), [](const cv::Point2f& p) {
          return p.x == 0.0f && p.y == 0.0f;
        });
    if (scoreboard_disabled_)
      scoreboard_perspective_polygion_.clear();
    std::cout << (scoreboard_disabled_ ? "Scoreboard overlay disabled by configured sentinel"
                                       : "Loaded scoreboard perspective polygon")
              << std::endl;
  } else if (key == "show-scoreboard") {
    show_scoreboard_ = !!std::atoi(prop.value.c_str());
  } else if (key == "scoreboard-projected-width") {
    scoreboard_projected_width_ = prop.value;
    scoreboard_.reset();
  } else if (key == "scoreboard-projected-height") {
    scoreboard_projected_height_ = prop.value;
    scoreboard_.reset();
  } else if (key == "scoreboard-scale") {
    scoreboard_scale_ = std::atof(prop.value.c_str());
    if (scoreboard_scale_ <= 0) {
      scoreboard_scale_ = 1.0;
    }
    scoreboard_.reset();
  } else if (key == "plot-play-tracking") {
    plot_play_tracking_ = !!std::atoi(prop.value.c_str());
  } else if (key == "plot-player-tracking") {
    plot_player_tracking_ = !!std::atoi(prop.value.c_str());
  } else if (key == "transform-object-meta") {
    transform_object_meta_ = !!std::atoi(prop.value.c_str());
  } else if (key == "runtime-output-max-width") {
    runtime_output_max_width_ = std::atol(prop.value.c_str());
  } else if (key == "runtime-output-max-height") {
    runtime_output_max_height_ = std::atol(prop.value.c_str());
  } else if (key == "fixed-edge-rotation-angle") {
    float angle = 0.0f;
    if (!parse_finite_float(prop.value, &angle)) {
      return false;
    }
    fixed_edge_rotation_angle_left_ = angle;
    fixed_edge_rotation_angle_right_ = angle;
  } else if (key == "fixed-edge-rotation-angle-left") {
    if (!parse_finite_float(prop.value, &fixed_edge_rotation_angle_left_)) {
      return false;
    }
  } else if (key == "fixed-edge-rotation-angle-right") {
    if (!parse_finite_float(prop.value, &fixed_edge_rotation_angle_right_)) {
      return false;
    }
  } else if (key == "shadow-lift") {
    float shadow_lift_percent = 0.0f;
    if (!parse_finite_float(prop.value, &shadow_lift_percent) || shadow_lift_percent < 0.0f ||
        shadow_lift_percent > 100.0f) {
      std::cerr << "Invalid shadow lift percentage: " << prop.value << std::endl;
      return false;
    }
    shadow_lift_percent_ = shadow_lift_percent;
  } else if (key == "no-crop") {
    // TODO: implement, needs to change caps too
    no_crop_ = !!std::atoi(prop.value.c_str());
  }
  return true;
}

BufferResult PlayCropperPriv::ProcessBuffer(GstBuffer* inbuf) {
  return Super::ProcessBuffer(inbuf);
}

void PlayCropperPriv::TransformObjectMetaForOutput(
    NvDsFrameMeta* frame_meta,
    float scale_w,
    float scale_h,
    const BBox& src_rect,
    float angle,
    const Point& anchor_point,
    const BBox& crop_box,
    const BBox& output_rect) {
  for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
    NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
    const NvOSD_RectParams original_rect_params = obj_meta->rect_params;
    const BBox source_box(
        original_rect_params.left,
        original_rect_params.top,
        original_rect_params.left + original_rect_params.width,
        original_rect_params.top + original_rect_params.height);

    BBox transformed_box;
    if (transform_box_to_output(
            source_box, scale_w, scale_h, src_rect, angle, anchor_point, crop_box, output_rect, &transformed_box)) {
      obj_meta->rect_params.left = transformed_box.left;
      obj_meta->rect_params.top = transformed_box.top;
      obj_meta->rect_params.width = transformed_box.width();
      obj_meta->rect_params.height = transformed_box.height();
      obj_meta->text_params.x_offset = transformed_box.left;
      obj_meta->text_params.y_offset = std::max(0.0f, transformed_box.top - 10.0f);
    } else {
      obj_meta->rect_params.left = 0;
      obj_meta->rect_params.top = 0;
      obj_meta->rect_params.width = 0;
      obj_meta->rect_params.height = 0;
      obj_meta->rect_params.border_width = 0;
      obj_meta->text_params.x_offset = 0;
      obj_meta->text_params.y_offset = 0;
    }

    NvBbox_Coords& detector_bbox_coords = obj_meta->detector_bbox_info.org_bbox_coords;
    if (bbox_coords_are_valid(detector_bbox_coords)) {
      const BBox detector_box(
          detector_bbox_coords.left,
          detector_bbox_coords.top,
          detector_bbox_coords.left + detector_bbox_coords.width,
          detector_bbox_coords.top + detector_bbox_coords.height);
      if (transform_box_to_output(
              detector_box, scale_w, scale_h, src_rect, angle, anchor_point, crop_box, output_rect, &transformed_box)) {
        set_bbox_coords(detector_bbox_coords, transformed_box);
      } else {
        clear_bbox_coords(detector_bbox_coords);
      }
    }

    NvBbox_Coords& tracker_bbox_coords = obj_meta->tracker_bbox_info.org_bbox_coords;
    if (bbox_coords_are_valid(tracker_bbox_coords)) {
      const BBox tracker_box(
          tracker_bbox_coords.left,
          tracker_bbox_coords.top,
          tracker_bbox_coords.left + tracker_bbox_coords.width,
          tracker_bbox_coords.top + tracker_bbox_coords.height);
      if (transform_box_to_output(
              tracker_box, scale_w, scale_h, src_rect, angle, anchor_point, crop_box, output_rect, &transformed_box)) {
        set_bbox_coords(tracker_bbox_coords, transformed_box);
      } else {
        clear_bbox_coords(tracker_bbox_coords);
      }
    }
  }

  frame_meta->source_frame_width = static_cast<guint>(output_rect.width());
  frame_meta->source_frame_height = static_cast<guint>(output_rect.height());
  frame_meta->pipeline_width = static_cast<guint>(output_rect.width());
  frame_meta->pipeline_height = static_cast<guint>(output_rect.height());
}

absl::Status PlayCropperPriv::GenerateOutput(
    NvDsBatchMeta* batch_meta,
    NvBufSurface* in_surface,
    NvBufSurface* out_surface) {
  absl::WriterMutexLock lk(&mu_process_);

  // Setup and initialization
  if (!in_surface->numFilled) {
    return absl::CancelledError("No surfaces were filled");
  }
  if (!in_surface->batchSize || in_surface->numFilled > in_surface->batchSize) {
    return absl::FailedPreconditionError("Playcropper input fill count exceeds its surface capacity");
  }
  if (in_surface->numFilled > out_surface->batchSize) {
    return absl::FailedPreconditionError("Playcropper output surface capacity is smaller than the filled input batch");
  }
  assert(cuda_stream_);

  // NppStreamContext nppStreamContext;
  // memset(&nppStreamContext, 0, sizeof(nppStreamContext));
  // nppStreamContext.hStream = cuda_stream_;
  // nppStreamContext.nStreamFlags = 0;
  // nppStreamContext.nCudaDeviceId = m_gpuId;

  HM_RETURN_IF_ERROR(hm::to_status(cudaSetDevice(m_gpuId)));

  const std::vector<std::optional<BBox>> tracking_boxes = get_object_boxes_by_frame(
      batch_meta, DsPlayTrackerInitParams::kPlayBoxClassIdBase, DsPlayTrackerInitParams::kPlayBoxClassIdBase);
  if (tracking_boxes.size() != batch_meta->num_frames_in_batch || tracking_boxes.size() != in_surface->numFilled) {
    return absl::FailedPreconditionError("Playcropper metadata does not match the filled input batch");
  }

  out_surface->numFilled = 0;

  size_t nr_surfaces_to_process = in_surface->numFilled;
  assert(nr_surfaces_to_process <= m_buffer_pool_config.max_buffers);

  NvDsFrameMetaList* frame_meta_list = batch_meta->frame_meta_list;

  std::unique_ptr<surface::Surface> display_surface;

  // Process each surface in the batch
  for (size_t batch_nr = 0; batch_nr < nr_surfaces_to_process; ++batch_nr, frame_meta_list = frame_meta_list->next) {
    assert(frame_meta_list);
    NvDsFrameMeta* frame_meta = (NvDsFrameMeta*)frame_meta_list->data;

    // Get input and output surfaces
#ifdef __aarch64__
    hm::surface::EglSurfaceMapper incoming_elg_surface_mapper(in_surface, batch_nr, /*read_only=*/true);
    HM_RETURN_IF_ERROR(hm::to_status(incoming_elg_surface_mapper.status()));
    hm::surface::Surface incoming_surface = incoming_elg_surface_mapper.get_surface();

    hm::surface::EglSurfaceMapper outgoing_elg_surface_mapper(out_surface, batch_nr, /*read_only=*/false);
    HM_RETURN_IF_ERROR(hm::to_status(outgoing_elg_surface_mapper.status()));
    hm::surface::Surface outgoing_surface = outgoing_elg_surface_mapper.get_surface();
#else
    hm::surface::Surface incoming_surface(&in_surface->surfaceList[batch_nr]);
    hm::surface::Surface outgoing_surface(&out_surface->surfaceList[batch_nr]);
#endif

    const size_t input_width = incoming_surface.width();
    const size_t input_height = incoming_surface.height();
    // ack this may vary on jetson I think
    const size_t output_width = outgoing_surface.width();
    const size_t output_height = outgoing_surface.height();
    // const size_t output_width = videoprep->output_width;
    // const size_t output_height = videoprep->output_width;

    // Calculate scaling factors
    FloatValue scale_w = float(input_width) / frame_meta->source_frame_width;
    FloatValue scale_h = float(input_height) / frame_meta->source_frame_height;

    // Get tracking box
    BBox tbox = tracking_boxes[batch_nr].has_value()
        ? *tracking_boxes[batch_nr]
        : make_null_tracking_box(&in_surface->surfaceList[batch_nr], &out_surface->surfaceList[batch_nr]);

    tbox.left *= scale_w;
    tbox.right *= scale_w;
    tbox.top *= scale_h;
    tbox.bottom *= scale_h;

    const FrameTransformGeometry transform = CalculateFrameTransformGeometry(
        input_width, input_height, tbox, no_crop_, fixed_edge_rotation_angle_left_, fixed_edge_rotation_angle_right_);

    // Calculate crop regions
#ifndef NDEBUG
    size_t tb_w = tbox.width();
    size_t tb_h = tbox.height();
    assert(tb_w <= input_width);
    assert(tb_h <= input_height);
#endif

#ifndef PLAYCROPPER_USE_ONE_KERNEL
    goto fallback;
#else
    const BBox output_rect(0, 0, (FloatValue)output_width, (FloatValue)output_height);

    // Check if we can use our optimized path
    NvBufSurfaceColorFormat color_format = incoming_surface->colorFormat;
#endif // PLAYCROPPER_USE_ONE_KERNEL
    if (color_format == NVBUF_COLOR_FORMAT_RGBA || color_format == NVBUF_COLOR_FORMAT_RGB ||
        color_format == NVBUF_COLOR_FORMAT_GRAY8) {
      XCUDA_RETURN_IF_ERROR(cudaMemsetAsync(
          outgoing_surface.dataptr(), 0, outgoing_surface.height() * outgoing_surface.pitch(), cuda_stream_));
      // Use the combined transform - no scratch surfaces needed!
      XCUDA_RETURN_IF_ERROR(combinedTransform(
          incoming_surface.get(),
          transform.source_rect,
          transform.angle,
          transform.anchor_point,
          transform.crop_box,
          outgoing_surface.get_mutable(),
          output_rect,
          shadow_lift_percent_,
          cuda_stream_));
    } else {
      // assert(false);
      // Fall back to original implementation for unsupported formats
      // fallback:
      std::cout << "playcropper no fallback" << std::endl;
      // TODO: Make the fallback work again
#if 0
      // Use the original three-step approach with minimal scratch surfaces
      hm::surface::SurfaceList::round_robin_iterator scratch_surface_iter = videoprep->priv->scratch_buffers.begin();

      // Step 1: Crop
      HM_RETURN_IF_ERROR(
          to_status(cropSurface(incoming_surface, transform.source_rect, *scratch_surface_iter, nppStreamContext)));
      // Step 2: Rotate
      auto in_surf_iter = scratch_surface_iter++;
      HM_RETURN_IF_ERROR(to_status(rotateNvBufSurfaceWithNPP(
          *in_surf_iter,
          BBox(0, 0, transform.source_rect.width(), transform.source_rect.height()),
          *scratch_surface_iter,
          BBox(0, 0, transform.source_rect.width(), transform.source_rect.height()),
          transform.angle,
          transform.anchor_point,
          nppStreamContext)));

      // Step 3: Final crop and resize
      HM_RETURN_IF_ERROR(to_status(cropAndResizeNvBufSurface(
          *scratch_surface_iter++, transform.crop_box, outgoing_surface, output_rect, nppStreamContext)));
#endif
    }

    // Scoreboard
    if (show_scoreboard_) {
      HM_RETURN_IF_ERROR(RenderScoreboard(incoming_surface, outgoing_surface, cuda_stream_));
    }
    if (show_ && !batch_nr) {
      // Render it inside the loop, but we'll display it after our cudaSynchronize
      display_surface = std::make_unique<surface::Surface>(incoming_surface);
      HM_RETURN_IF_ERROR(RenderDisplayMeta(*display_surface, frame_meta, cuda_stream_));
    }
    if (transform_object_meta_) {
      TransformObjectMetaForOutput(
          frame_meta,
          scale_w,
          scale_h,
          transform.source_rect,
          transform.angle,
          transform.anchor_point,
          transform.crop_box,
          output_rect);
    }
    ++frame_count_;
  }

  // Synchronize stream
  if (cuda_stream_) {
    cudaStreamSynchronize(cuda_stream_);
  }

  if (show_ && display_surface) {
    // If rendering, only render opne per batch and do it after the main cuda synchronize
    // if (batch_meta->)
    // render("Play Cropper", &out_surface->surfaceList[batch_nr], cuda_stream_);
    // cudaStreamSynchronize(cuda_stream_);
    render("Play Tracking", &display_dest_params_, cuda_stream_);
  }

  out_surface->numFilled = nr_surfaces_to_process;

  return absl::OkStatus();
}

absl::Status PlayCropperPriv::LoadScoreboardPerspectiveFromConfig() {
  if (config_file_.empty()) {
    return absl::NotFoundError("No playcropper config-file is available for scoreboard reload");
  }
  std::filesystem::path config_path(config_file_);
  if (std::filesystem::is_directory(config_path)) {
    config_path /= "config.yaml";
  }
  auto loaded_config = stitching::load_game_config_file(config_path);
  if (!loaded_config.ok()) {
    return loaded_config.status();
  }
  if (!loaded_config->has_value()) {
    return absl::NotFoundError(TO_STRING("Scoreboard config file does not exist: " << config_path.string()));
  }

  try {
    YAML::Node cfg = **loaded_config;
    YAML::Node polygon = cfg["rink"]["scoreboard"]["perspective_polygon"];
    if (!polygon || !polygon.IsSequence() || polygon.size() != 4) {
      return absl::NotFoundError("Scoreboard perspective_polygon is not configured yet");
    }

    std::stringstream ss;
    for (size_t i = 0; i < polygon.size(); ++i) {
      YAML::Node point = polygon[i];
      if (!point || !point.IsSequence() || point.size() != 2) {
        return absl::InvalidArgumentError("Scoreboard perspective_polygon must contain four x,y points");
      }
      if (i) {
        ss << ',';
      }
      ss << point[0].as<float>() << ',' << point[1].as<float>();
    }
    scoreboard_perspective_polygion_.clear();
    std::vector<std::string> points = absl::StrSplit(ss.str(), ',');
    if (points.size() != 8) {
      return absl::InternalError("Scoreboard perspective_polygon reload produced an invalid point list");
    }
    for (size_t i = 0, n = points.size() >> 1; i < n; ++i) {
      const size_t index = i << 1;
      scoreboard_perspective_polygion_.emplace_back(
          cv::Point2f(std::atof(points[index].c_str()), std::atof(points.at(index + 1).c_str())));
    }
    scoreboard_disabled_ = std::all_of(
        scoreboard_perspective_polygion_.begin(), scoreboard_perspective_polygion_.end(), [](const cv::Point2f& p) {
          return p.x == 0.0f && p.y == 0.0f;
        });
    if (scoreboard_disabled_)
      scoreboard_perspective_polygion_.clear();
    scoreboard_.reset();
    std::cout << (scoreboard_disabled_ ? "Scoreboard overlay disabled by config reload"
                                       : "Loaded scoreboard perspective polygon from config reload")
              << std::endl;
    return absl::OkStatus();
  } catch (const YAML::Exception& ex) {
    return absl::InternalError(
        TO_STRING("Failed to load scoreboard perspective from \"" << config_path.string() << "\": " << ex.what()));
  }
}

absl::Status PlayCropperPriv::EnsureScoreboardPerspectiveConfigured(surface::Surface stitched_surface) {
  if (scoreboard_disabled_ || !scoreboard_perspective_polygion_.empty()) {
    return absl::OkStatus();
  }

  absl::Status reload_status = LoadScoreboardPerspectiveFromConfig();
  if (reload_status.ok()) {
    return absl::OkStatus();
  }
  if (reload_status.code() != absl::StatusCode::kNotFound) {
    return reload_status;
  }
  if (scoreboard_configure_attempted_) {
    return absl::OkStatus();
  }
  scoreboard_configure_attempted_ = true;

  if (config_file_.empty()) {
    return absl::NotFoundError("No playcropper config-file is available for scoreboard configuration");
  }

  std::filesystem::path game_dir(config_file_);
  std::error_code ec;
  if (!std::filesystem::is_directory(game_dir, ec) &&
      (game_dir.extension() == ".yaml" || game_dir.extension() == ".yml")) {
    game_dir = game_dir.parent_path();
  }
  if (game_dir.empty()) {
    return absl::InvalidArgumentError("Could not determine game directory from playcropper config-file");
  }

  const std::filesystem::path stitched_image = game_dir / "s.png";
  if (!std::filesystem::exists(stitched_image, ec) || ec || std::filesystem::file_size(stitched_image, ec) == 0 || ec) {
    HM_RETURN_IF_ERROR(stitching::save_stitched_image(game_dir.string(), stitched_surface));
  }

  HM_RETURN_IF_ERROR(stitching::configure_scoreboard(game_dir.string()));
  return LoadScoreboardPerspectiveFromConfig();
}

absl::Status PlayCropperPriv::RenderDisplayMeta(
    surface::Surface surface,
    const NvDsFrameMeta* frame_meta,
    cudaStream_t stream) {
  if (!font_cache_) {
    font_cache_ = draw_display::get_or_create_font_cache();
  }
  if (!display_surface_) {
    if (surface.get_image_format() != imageFormat::IMAGE_RGBA8) {
      return absl::FailedPreconditionError("Unsupported image format for RenderDisplayMeta");
    }
    const size_t ww = static_cast<size_t>(render_scale_ * surface.width());
    const size_t hh = static_cast<size_t>(render_scale_ * surface.height());
    display_surface_ = std::make_unique<CudaMat<uchar4>>(/*B=*/1, ww, hh);
    memset(&display_dest_params_, 0, sizeof(display_dest_params_));
    display_dest_params_.width = display_surface_->width();
    display_dest_params_.height = display_surface_->height();
    display_dest_params_.pitch = display_surface_->pitch();
    display_dest_params_.colorFormat = surface->colorFormat;
    display_dest_params_.dataPtr = display_surface_->data();
  }

  NppStreamContext nppStreamContext;
  memset(&nppStreamContext, 0, sizeof(nppStreamContext));
  nppStreamContext.hStream = stream;
  nppStreamContext.nStreamFlags = 0;
  nppStreamContext.nCudaDeviceId = m_gpuId;

  HM_RETURN_IF_ERROR(to_status(cropAndResizeNvBufSurface(
      surface,
      hm::BBox(0, 0, surface.width(), surface.height()),
      &display_dest_params_,
      hm::BBox(0, 0, display_dest_params_.width, display_dest_params_.height),
      nppStreamContext)));

  if (plot_play_tracking_) {
    NvDisplayMetaList* dm_list = frame_meta->display_meta_list;
    while (dm_list) {
      NvDsDisplayMeta* display_meta = (NvDsDisplayMeta*)dm_list->data;
      HM_RETURN_IF_ERROR(draw_display_meta(&display_dest_params_, display_meta, font_cache_, render_scale_, stream));
      dm_list = dm_list->next;
    }
  }

  if (plot_player_tracking_) {
    std::vector<NvDsObjectMeta*> tracked_player_metas;
    tracked_player_metas.reserve(frame_meta->num_obj_meta);
    for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
      NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
      if (obj_meta->object_id == UNTRACKED_OBJECT_ID || obj_meta->class_id != 0) {
        continue;
      }
      tracked_player_metas.push_back(obj_meta);
    }
    if (!tracked_player_metas.empty()) {
      HM_RETURN_IF_ERROR(
          draw_object_meta(&display_dest_params_, tracked_player_metas, font_cache_, render_scale_, stream));
    }
  }

  // const PlayTrackerPayload* playtracker_payload = PlayTrackerPayload::get_payload<PlayTrackerPayload>(frame_meta);
  // if (playtracker_payload) {
  //   usleep(0);
  // }

  return absl::OkStatus();
}

absl::Status PlayCropperPriv::RenderScoreboard(
    surface::Surface in_surface,
    surface::Surface out_surface,
    cudaStream_t stream) {
  if (scoreboard_perspective_polygion_.empty()) {
    HM_RETURN_IF_ERROR(EnsureScoreboardPerspectiveConfigured(in_surface));
  }
  if (!scoreboard_ && !scoreboard_perspective_polygion_.empty()) {
    const auto resolve_dimension = [](const std::string& value, float fallback, guint extent) -> absl::StatusOr<int> {
      if (value.empty()) {
        return std::max(1, static_cast<int>(fallback));
      }
      const bool is_percent = value[0] == '%';
      const char* start = value.c_str() + (is_percent ? 1 : 0);
      char* end = nullptr;
      errno = 0;
      const float parsed = std::strtof(start, &end);
      if (start == end || errno != 0 || end == nullptr || *end != '\0' || parsed <= 0) {
        return absl::InvalidArgumentError(TO_STRING("Invalid scoreboard dimension: " << value));
      }
      const float pixels = is_percent ? (parsed / 100.0f) * extent : parsed;
      if (pixels <= 0) {
        return absl::InvalidArgumentError(TO_STRING("Invalid scoreboard dimension: " << value));
      }
      return std::max(1, static_cast<int>(pixels));
    };
    auto scoreboard_width_or = resolve_dimension(
        scoreboard_projected_width_, out_surface.width() * scoreboard_width_ratio_, out_surface.width());
    if (!scoreboard_width_or.ok()) {
      return scoreboard_width_or.status();
    }
    auto scoreboard_height_or = resolve_dimension(
        scoreboard_projected_height_, out_surface.height() * scoreboard_height_ratio_, out_surface.height());
    if (!scoreboard_height_or.ok()) {
      return scoreboard_height_or.status();
    }
    const int scoreboard_width = std::max(1, static_cast<int>(scoreboard_scale_ * scoreboard_width_or.value()));
    const int scoreboard_height = std::max(1, static_cast<int>(scoreboard_scale_ * scoreboard_height_or.value()));
    scoreboard_ = std::make_unique<hm::scoreboard::Scoreboard<uchar4>>(
        scoreboard_perspective_polygion_, scoreboard_width, scoreboard_height);
  }
  if (scoreboard_) {
    const bool rewarp = frame_count_ % scoreboard_warp_interval_ == 0;
    HM_RETURN_IF_ERROR(scoreboard_->forward_prod(in_surface, out_surface, rewarp, cuda_stream_));
  }
  return absl::OkStatus();
}

} // namespace playcropper
} // namespace hm
