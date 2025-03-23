#include "hstream/src/gst-plugins/gst-videoprep/playcropper/playcropper.h"
#include "cupano/pano/cudaMat.h"
#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/preputils.h"
#include "hstream/src/gst-plugins/gst-videoprep/playcropper/cudaPlayCropper.h"
// #include "hstream/src/gst-plugins/gst-videoprep/playtracker/playtracker_payload.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/draw_display/DrawDisplayMeta.h"

#include "deepstream/sources/includes/nvbufsurface.h"
#include "hstream/src/libs/draw_display/Fonts.h"
#include "nvdsmeta.h"

#include "absl/strings/str_split.h"

#include <cmath>
#include <vector>

#include <absl/status/status.h>
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

} // namespace

absl::Status PlayCropperPriv::PreCapsInit(DSCustom_CreateParams* params) {
  // Not an in-place transform
  m_inVideoFmt = GST_VIDEO_FORMAT_RGBA;
  m_outVideoFmt = GST_VIDEO_FORMAT_RGBA;
  // videoprep::GstVideoPrep* videoprep = GST_VIDEOPREP(params->m_element);
  return Super::PreCapsInit(params);
};

absl::Status PlayCropperPriv::PostCapsInit(DSCustom_CreateParams* params) {
  m_transformMode = true;

  return Super::PostCapsInit(params);
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
  if (prop.key == "show") {
    show_ = !!std::atol(prop.value.c_str());
  } else if (prop.key == "render-scale") {
    render_scale_ = std::atof(prop.value.c_str());
    if (render_scale_ == 0) {
      std::cerr << "Invalid render scale: " << render_scale_ << std::endl;
      return false;
    }
  } else if (prop.key == "scoreboard-perspective-polygon") {
    std::cout << "GOT scoreboard-perspective-polygon!" << std::endl;
    scoreboard_perspective_polygion_.clear();
    std::vector<std::string> points = absl::StrSplit(prop.value, ',');
    assert(points.size() == 8);
    for (size_t i = 0, n = points.size() >> 1; i < n; ++i) {
      const size_t index = i << 1;
      scoreboard_perspective_polygion_.emplace_back(
          cv::Point2f(std::atof(points[index].c_str()), std::atof(points.at(index + 1).c_str())));
    }
    assert(scoreboard_perspective_polygion_.size() == 4);
  } else if (prop.key == "show-scoreboard") {
    show_scoreboard_ = !!std::atoi(prop.value.c_str());
  } else if (prop.key == "plot-play-tracking") {
    plot_play_tracking_ = !!std::atoi(prop.value.c_str());
  } else if (prop.key == "plot-player-tracking") {
    plot_player_tracking_ = !!std::atoi(prop.value.c_str());
  } else if (prop.key == "fixed-edge-rotation-angle") {
    fixed_edge_rotation_angle_ = std::atof(prop.value.c_str());
  }
  return true;
}

BufferResult PlayCropperPriv::ProcessBuffer(GstBuffer* inbuf) {
  return Super::ProcessBuffer(inbuf);
}

absl::Status PlayCropperPriv::GenerateOutput(
    NvDsBatchMeta* batch_meta,
    NvBufSurface* in_surface,
    NvBufSurface* out_surface) {
  
  absl::ReaderMutexLock lk(&mu_process_);

  // Setup and initialization
  if (!in_surface->numFilled) {
    return absl::CancelledError("No surfaces were filled");
  }
  assert(in_surface->numFilled == out_surface->batchSize);
  assert(cuda_stream_);

  NppStreamContext nppStreamContext;
  memset(&nppStreamContext, 0, sizeof(nppStreamContext));
  nppStreamContext.hStream = cuda_stream_;
  nppStreamContext.nStreamFlags = 0;
  nppStreamContext.nCudaDeviceId = m_gpuId;

  HM_RETURN_IF_ERROR(hm::to_status(cudaSetDevice(m_gpuId)));

  const std::vector<BBox> tracking_boxes = get_tracking_boxes(batch_meta);

  out_surface->numFilled = 0;

  assert(tracking_boxes.empty() || tracking_boxes.size() == in_surface->numFilled);
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
    hm::surface::Surface incoming_surface = incoming_elg_surface_mapper.get_surface();

    hm::surface::EglSurfaceMapper outgoing_elg_surface_mapper(out_surface, batch_nr, /*read_only=*/false);
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
    BBox tbox = !tracking_boxes.empty()
        ? tracking_boxes.at(batch_nr)
        : make_null_tracking_box(&in_surface->surfaceList[batch_nr], &out_surface->surfaceList[batch_nr]);

    tbox.left *= scale_w;
    tbox.right *= scale_w;
    tbox.top *= scale_h;
    tbox.bottom *= scale_h;

    // FloatValue width_for_ratio = 1.0;
    // const playtracker::PlayTrackerPayload* ptpayload =
    //     playtracker::PlayTrackerPayload::get_payload<playtracker::PlayTrackerPayload>(frame_meta);
    // if (ptpayload) {
    //   width_for_ratio = FloatValue(ptpayload->arena_box().width()) / frame_meta->source_frame_width;
    // }

    // Calculate rotation angle
    float angle = 0.0f;
    const float max_angle = fixed_edge_rotation_angle_;
    const float half_width = float(frame_meta->source_frame_width) / 2;
    const float tcx = tbox.center().x;
    if (tcx < half_width) {
      float pct = 1.0 - tcx / half_width;
      angle = max_angle * pct;
    } else if (tcx > half_width) {
      float pct = (half_width - tcx) / half_width;
      angle = max_angle * pct;
    }

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
    const BBox input_rect(0, 0, input_width, input_height);
    const int x_center = tbox.center().x;

    long pre_rotate_size_width = 0 /* ??? */;
    // long pre_rotate_size_width = videoprep->pre_rotate_size.width;
    FloatValue min_width_per_side = pre_rotate_size_width / 2;
    min_width_per_side = std::max(min_width_per_side, tbox.width() / 2);
    FloatValue clip_left = std::max(input_rect.left, x_center - min_width_per_side);
    FloatValue clip_right = std::min(input_rect.right, x_center + min_width_per_side);
    BBox extra_width_src_rect(clip_left, input_rect.top, clip_right, input_rect.bottom);

    BBox new_tbox = tbox;
    new_tbox.left -= extra_width_src_rect.left;
    new_tbox.right -= extra_width_src_rect.left;
    // assert(new_tbox.left >= 0 && new_tbox.top >= 0);
    // assert(new_tbox.right <= extra_width_src_rect.width());

    const BBox output_rect(0, 0, (FloatValue)output_width, (FloatValue)output_height);

    Point anchor_point = new_tbox.center();

    // Check if we can use our optimized path
    NvBufSurfaceColorFormat color_format = incoming_surface->colorFormat;
#endif // PLAYCROPPER_USE_ONE_KERNEL
    if (color_format == NVBUF_COLOR_FORMAT_RGBA || color_format == NVBUF_COLOR_FORMAT_RGB ||
        color_format == NVBUF_COLOR_FORMAT_GRAY8) {
      // Use the combined transform - no scratch surfaces needed!
      XCUDA_RETURN_IF_ERROR(combinedTransform(
          incoming_surface.get(),
          extra_width_src_rect,
          angle,
          anchor_point,
          new_tbox,
          outgoing_surface.get_mutable(),
          output_rect,
          nppStreamContext));
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
          to_status(cropSurface(incoming_surface, extra_width_src_rect, *scratch_surface_iter, nppStreamContext)));
      // Step 2: Rotate
      auto in_surf_iter = scratch_surface_iter++;
      HM_RETURN_IF_ERROR(to_status(rotateNvBufSurfaceWithNPP(
          *in_surf_iter,
          BBox(0, 0, extra_width_src_rect.width(), extra_width_src_rect.height()),
          *scratch_surface_iter,
          BBox(0, 0, extra_width_src_rect.width(), extra_width_src_rect.height()),
          angle,
          anchor_point,
          nppStreamContext)));

      // Step 3: Final crop and resize
      HM_RETURN_IF_ERROR(to_status(cropAndResizeNvBufSurface(
          *scratch_surface_iter++, new_tbox, outgoing_surface, output_rect, nppStreamContext)));
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
    cudaStreamSynchronize(cuda_stream_);
    render("Play Tracking", &display_dest_params_, cuda_stream_);
  }

  out_surface->numFilled = nr_surfaces_to_process;

  return absl::OkStatus();
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
    std::vector<NvDsObjectMeta*> object_metas =
        glist_to_vect<NvDsObjectMeta>(frame_meta->obj_meta_list, frame_meta->num_obj_meta);
    // for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
    //   NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
    //   if (obj_meta->object_id == UNTRACKED_OBJECT_ID) {
    //     // Don't draw untracked objects
    //     continue;
    //   }
    //   HM_RETURN_IF_ERROR(draw_object_meta(&display_dest_params_, obj_meta, font_cache_, render_scale_, stream));
    // }
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
  if (!scoreboard_ && !scoreboard_perspective_polygion_.empty()) {
    scoreboard_ = std::make_unique<hm::scoreboard::Scoreboard<uchar4>>(
        scoreboard_perspective_polygion_,
        out_surface.width() * scoreboard_width_ratio_,
        out_surface.height() * scoreboard_height_ratio_);
  }
  if (scoreboard_) {
    const bool rewarp = frame_count_ % scoreboard_warp_interval_ == 0;
    HM_RETURN_IF_ERROR(scoreboard_->forward_prod(in_surface, out_surface, rewarp, cuda_stream_));
  }
  return absl::OkStatus();
}

} // namespace playcropper
} // namespace hm
