#include "gstvideoprep.h"

#include <cuda_runtime.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <npp.h>
#include <nvbufsurface.h>
#include <cmath>
#include "nvbufsurface.h"
#include "nvdsmeta.h"
#include "preputils.h"

#include <assert.h>
#include <cuda.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include "nvbufsurface.h"

#if defined(__aarch64__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "cudaEGL.h"
#endif
#include "playcropper.h"

// #define SCRATCH_USE_ALIGNED_PITCH

namespace hm {
namespace playcropper {

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

bool PlayCropperPriv::PreCapsInit(DSCustom_CreateParams* params) {
  // Not an in-place transform
#ifdef NEW_VIDEOPREP
  m_transformMode = true;
#endif
  m_inVideoFmt = GST_VIDEO_FORMAT_RGBA;
  m_outVideoFmt = GST_VIDEO_FORMAT_RGBA;
  // videoprep::GstVideoPrep* videoprep = GST_VIDEOPREP(params->m_element);
  // videoprep->num_batch_buffers /= 2;
  return Super::PreCapsInit(params);
};

bool PlayCropperPriv::PostCapsInit(DSCustom_CreateParams* params) {
  return Super::PostCapsInit(params);
}

gint PlayCropperPriv::AllocateScratchBuffers(videoprep::GstVideoPrep* videoprep) {
  cudaError_t cudaErr;

  assert(videoprep->input_width && videoprep->input_height);

  cudaErr = cudaSetDevice(videoprep->gpu_id);
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
  return 0;
}

BufferResult PlayCropperPriv::ProcessBuffer(GstBuffer* inbuf) {
  return Super::ProcessBuffer(inbuf);
}

cudaError PlayCropperPriv::GenerateOutput(
    NvDsBatchMeta* batch_meta,
    videoprep::GstVideoPrep* videoprep,
    NvBufSurface* in_surface,
    NvBufSurface* out_surface) {
  static size_t counter = 0;
  std::cout << "PlayCropperPriv::GenerateOutput: " << counter++ << std::endl;
  cudaError err = cudaSuccess;
  assert(cudaGetLastError() == cudaSuccess);

  // Debugging sanity check
  assert(in_surface->numFilled == out_surface->batchSize);

  assert(videoprep->stream);

  NppStreamContext nppStreamContext;
  memset(&nppStreamContext, 0, sizeof(nppStreamContext));
  nppStreamContext.hStream = videoprep->stream; // Assign the CUDA stream
  nppStreamContext.nStreamFlags = 0; // No special flags
  nppStreamContext.nCudaDeviceId = videoprep->gpu_id; // Default queue size

  err = cudaSetDevice(videoprep->gpu_id);
  assert(err == cudaSuccess);

  const std::vector<BBox> tracking_boxes = get_tracking_boxes(batch_meta);

  out_surface->numFilled = 0;

  // TODO: what do we do about this mismatch???
  assert(tracking_boxes.empty() || tracking_boxes.size() == in_surface->numFilled);
  // size_t nr_surfaces_to_process = std::min(tracking_boxes.size(), (size_t)out_surface->batchSize);
  size_t nr_surfaces_to_process = in_surface->numFilled;
  assert(nr_surfaces_to_process <= videoprep->num_batch_buffers);

  NvDsFrameMetaList* frame_meta_list = batch_meta->frame_meta_list;

  for (size_t batch_nr = 0; batch_nr < nr_surfaces_to_process; ++batch_nr, frame_meta_list = frame_meta_list->next) {
    assert(frame_meta_list);
    NvDsFrameMeta* frame_meta = (NvDsFrameMeta*)frame_meta_list->data;

#ifdef __aarch64__
    hm::surface::EglSurfaceMapper incoming_elg_surface_mapper(in_surface, batch_nr, /*read_only=*/true);
    hm::surface::Surface incoming_surface = incoming_elg_surface_mapper.get_surface();
#else
    hm::surface::Surface incoming_surface(&in_surface->surfaceList[batch_nr]);
#endif

    // FloatValue pipeline_width =
    //     frame_meta->pipeline_width ? frame_meta->pipeline_width : frame_meta->source_frame_width;
    // FloatValue pipeline_height =
    //     frame_meta->pipeline_height ? frame_meta->pipeline_height : frame_meta->source_frame_height;

    // FloatValue pipeline_width = frame_meta->pipeline_width ? frame_meta->pipeline_width : incoming_surface->width;
    // FloatValue pipeline_height = frame_meta->pipeline_height ? frame_meta->pipeline_height :
    // incoming_surface->height;

    FloatValue scale_w = float(videoprep->input_width) / frame_meta->source_frame_width;
    FloatValue scale_h = float(videoprep->input_height) / frame_meta->source_frame_height;

    BBox tbox = !tracking_boxes.empty()
        ? tracking_boxes.at(batch_nr)
        : make_null_tracking_box(&in_surface->surfaceList[batch_nr], &out_surface->surfaceList[batch_nr]);

    tbox.left *= scale_w;
    tbox.right *= scale_w;
    tbox.top *= scale_h;
    tbox.bottom *= scale_h;

    float angle;
    const float max_angle = 30.0;
    const float half_width = float(frame_meta->source_frame_width) / 2;
    const float tcx = tbox.center().x;
    if (tcx < half_width) {
      float pct = 1.0 - tcx / half_width;
      angle = max_angle * pct;
    } else if (tcx > half_width) {
      float pct = (half_width - tcx) / half_width;
      angle = max_angle * pct;
    }

    // tbox_aar = tbox.width() / tbox.height();
    size_t tb_w = tbox.width();
    size_t tb_h = tbox.height();
    assert(tb_w <= videoprep->input_width);
    assert(tb_h <= videoprep->input_height);

    const BBox input_rect(0, 0, videoprep->input_width, videoprep->input_height);

    const int x_center = tbox.center().x;

    // const FloatValue tbox_aar = tbox.width() / tbox.height();

    assert(tbox.height() <= videoprep->pre_rotate_size.height);
    // this means our pre-rotate allocation is smaller than the tbox, so that's a bug when calculating pre_rotate_size
    assert(tbox.width() <= videoprep->pre_rotate_size.width);

    // hm::WHDims src_size{.width = (FloatValue)videoprep->input_width, .height = (FloatValue)videoprep->input_height};
    // hm::WHDims output_size{.width=tbox.width(), .height=tbox.height()};
    // auto pre_rotate_size = get_box_size_necessary_for_rotations(src_size, output_size);
    // const FloatValue min_width_per_side = pre_rotate_size.width / 2;
    FloatValue min_width_per_side = videoprep->pre_rotate_size.width / 2;
    min_width_per_side = std::max(min_width_per_side, tbox.width() / 2);
    FloatValue clip_left = std::max(input_rect.left, x_center - min_width_per_side);
    // FloatValue clip_right = std::min(input_rect.right - 1, x_center + min_width_per_side);
    FloatValue clip_right = std::min(input_rect.right, x_center + min_width_per_side);
    BBox extra_width_src_rect(clip_left, input_rect.top, clip_right, input_rect.bottom);

    BBox new_tbox = tbox;
    new_tbox.left -= extra_width_src_rect.left;
    new_tbox.right -= extra_width_src_rect.left;
    assert(new_tbox.left >= 0 && new_tbox.top >= 0);
    assert(new_tbox.right <= extra_width_src_rect.width());

    const BBox dst_box(0, 0, extra_width_src_rect.width(), extra_width_src_rect.height());
    assert(dst_box.left == 0.0);
    assert(dst_box.top == 0.0);

    NppStatus np_status = NppStatus::NPP_SUCCESS;
    (void)np_status;

    hm::surface::SurfaceList::round_robin_iterator scratch_surface_iter = videoprep->priv->scratch_buffers.begin();

    hm::surface::Surface out_surf(&out_surface->surfaceList[batch_nr]);

    assert(batch_nr < in_surface->numFilled);

#if 1
    {
      np_status = cropSurface(
          incoming_surface,
          /*src_rect=*/extra_width_src_rect,
          *scratch_surface_iter,
          /*clear_output_surface=*/false,
          nppStreamContext);
    }
    assert(np_status == NppStatus::NPP_SUCCESS);
#endif
    // videoprep->priv->render(std::string("First Crop"), *scratch_surface_iter, nppStreamContext.hStream);
    // #ifndef NDEBUG
    FloatValue tbox_ar = tbox.width() / tbox.height();
    FloatValue new_tbox_ar = new_tbox.width() / new_tbox.height();
    const BBox output_rect(0, 0, (FloatValue)videoprep->output_width, (FloatValue)videoprep->output_height);
    // FloatValue output_ar = output_rect.width() / output_rect.height();
    assert(isClose(tbox_ar, new_tbox_ar, 1e-6f, 0.001));
    // assert(isClose(new_tbox_ar, output_ar, 1e-6f, 0.001));
    // #endif

    // We just rotate the whole thing around the point
    // that is effectively the center of the tracking box
#if 1
    {
      Point anchor_point = new_tbox.center();
      // anchor_point.x = 0;
      // anchor_point.y = 0;
      auto in_surf_iter = scratch_surface_iter++;
      np_status = rotateNvBufSurfaceWithNPP(
          *in_surf_iter,
          dst_box,
          *scratch_surface_iter,
          dst_box,
          angle,
          /*anchor_point=*/anchor_point,
          nppStreamContext);
      assert(np_status == NppStatus::NPP_SUCCESS);
      // videoprep->priv->render(std::string("Rotate"), *in_surf_iter, nppStreamContext.hStream);
    }
#endif

#if 1
    {
#ifdef __aarch64__
      hm::surface::EglSurfaceMapper outgoing_elg_surface_mapper(out_surface, batch_nr, /*read_only=*/false);
      hm::surface::Surface outgoing_surface = outgoing_elg_surface_mapper.get_surface();
#else
      hm::surface::Surface outgoing_surface(&out_surface->surfaceList[batch_nr]);
#endif
      np_status = cropAndResizeNvBufSurface(
          /*srcSurface=*/*scratch_surface_iter++,
          /*src_rect=*/new_tbox,
          outgoing_surface,
          /*dest_rect=*/output_rect,
          nppStreamContext);
      assert(np_status == NppStatus::NPP_SUCCESS);
      // videoprep->priv->render(std::string("Outgoing resized"), outgoing_surface, nppStreamContext.hStream);
    }
#endif
    // hm::glist_visitor<NvDsDisplayMeta>(frame_meta->display_meta_list, [&](NvDsDisplayMeta* display_meta) {
    //   for (size_t i = 0; i < display_meta->num_rects; ++i) {
    //     auto& rect = display_meta->rect_params[i];
    //     rect.left -= extra_width_src_rect.left;
    //     rect.top -= extra_width_src_rect.top;
    //   }
    // });
    // hm::glist_visitor<NvDsObjectMeta>(frame_meta->object_meta_list, [&](NvDsObjectMeta* object_meta) {
    //   for (size_t i = 0; i < display_meta->num_rects; ++i) {
    //     auto& rect = display_meta->rect_params[i];
    //     rect.left -= extra_width_src_rect.left;
    //     rect.top -= extra_width_src_rect.top;
    //   }
    // });
  }

  if (videoprep->stream) {
    cudaStreamSynchronize(videoprep->stream);
  }

  out_surface->numFilled = nr_surfaces_to_process;

  // videoprep::videoprep_add_surface_meta(videoprep->out_gst_buf, nr_surfaces_to_process, videoprep->source_id);
#if 0
  surface_meta->num_filled_surfaces = nr_surfaces_to_process;
  surface_meta->source_id = videoprep->source_id;
  NvDsMeta* meta = NULL;
  meta = gst_buffer_add_nvds_meta(
      videoprep->out_gst_buf, surface_meta, NULL, videoprep_meta_copy_func, videoprep_meta_release_func);

  meta->meta_type = NVDS_DEWARPER_GST_META;
  meta->gst_to_nvds_meta_transform_func = videoprep_gst_to_nvds_meta_transform_func;
  meta->gst_to_nvds_meta_release_func = videoprep_gst_nvds_meta_release_func;
#endif
  return err;
}

static void gst_playcropper_class_init(GstVideoPrepPlayCropperClass* klass) {
  gst_videoprep_class_init_base(klass);
}

static void gst_playcropper_init(GstVideoPrepPlayCropper* playcropper) {
  gst_videoprep_init_base(playcropper);
}

#define gst_playcropper_parent_class parent_class
G_DEFINE_TYPE(GstVideoPrepPlayCropper, gst_playcropper, GST_TYPE_BASE_TRANSFORM);

} // namespace playcropper
} // namespace hm
