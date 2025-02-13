#include "gstvideoprep.h"

#include <cuda_runtime.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <npp.h>
#include <nvbufsurface.h>
#include <cmath>
#include <map>
#include "nvbufsurface.h"
#include "nvds_dewarper_meta.h"
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
#include "stitcher.h"

namespace hm {
namespace stitcher {

StitcherPriv::~StitcherPriv() {
  stitcher_.reset();
}

bool StitcherPriv::SetInitParams(DSCustom_CreateParams* params) {
  videoprep::GstVideoPrep* videoprep = GST_VIDEOPREP(params->m_element);
  if (!Super::SetInitParams(params)) {
    return false;
  }
  assert(!stitcher_);
  // CudaStitchPano(int batch_size, int num_levels, const ControlMasks& control_masks, bool match_exposure = false);
  hm::pano::ControlMasks control_masks;
  if (!control_masks.load(videoprep->config_file)) {
    return false;
  }
  stitcher_ = std::make_unique<hm::pano::cuda::CudaStitchPano<uchar4, float4>>(
      videoprep->num_batch_buffers, /*num+_levels=*/6, control_masks, /*match_exposure=*/true);
  if (!stitcher_->status().ok()) {
    return false;
  }
  videoprep->output_width = stitcher_->canvas_width();
  videoprep->output_height = stitcher_->canvas_height();
  return true;
}

cudaError StitcherPriv::GenerateOutput(
    NvDsBatchMeta* batch_meta,
    videoprep::GstVideoPrep* videoprep,
    NvBufSurface* in_surface,
    NvBufSurface* out_surface) {
  cudaError err = cudaSuccess;

  assert(cudaGetLastError() == cudaSuccess);

  // NvDewarperSurfaceMeta* surface_meta = (NvDewarperSurfaceMeta*)calloc(1, sizeof(NvDewarperSurfaceMeta));

  assert(in_surface->batchSize % 2 == 0);
  assert(in_surface->numFilled % 2 == 0);
  // assert(in_surface->isContiguous);

  //  source_id -> frame_number -> NvBufSurfaceParams*
  std::map<int, std::vector<NvBufSurfaceParams*>> source_frame_surfaces;
  // for (int i = 0; i < in_surface->numFilled; ++i) {
  //   NvBufSurfaceParams* params = &in_surface->surfaceList[i];

  // }

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
  assert(tracking_boxes.size() == in_surface->numFilled);
  const size_t nr_surfaces_to_process = std::min(tracking_boxes.size(), (size_t)out_surface->batchSize);
  assert(nr_surfaces_to_process <= videoprep->num_batch_buffers);

  NvDsFrameMetaList* frame_meta_list = batch_meta->frame_meta_list;

  for (size_t batch_nr = 0; batch_nr < nr_surfaces_to_process; ++batch_nr, frame_meta_list = frame_meta_list->next) {
    assert(frame_meta_list);
    NvDsFrameMeta* frame_meta = (NvDsFrameMeta*)frame_meta_list->data;
    assert(frame_meta->num_surfaces_per_frame == 1);
    source_frame_surfaces[frame_meta->source_id].emplace_back(&in_surface->surfaceList[frame_meta->surface_index]);
  }

#if 0
  for (size_t batch_nr = 0; batch_nr < nr_surfaces_to_process; ++batch_nr, frame_meta_list = frame_meta_list->next) {
    assert(frame_meta_list);
    NvDsFrameMeta* frame_meta = (NvDsFrameMeta*)frame_meta_list->data;


    
    // Set the new pipeline surface size for anyone who cares downstream
    frame_meta->pipeline_width = videoprep->output_width;
    frame_meta->pipeline_height = videoprep->output_height;
#ifdef __aarch64__
    hm::surface::EglSurfaceMapper incoming_elg_surface_mapper(in_surface, batch_nr);
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

#if 1
    float angle;
    const float max_angle = 30.0;
    const float half_width = float(frame_meta->source_frame_width) / 2;
    const float tcx = tracking_boxes.at(batch_nr).center().x;
    if (tcx < half_width) {
      float pct = 1.0 - tcx / half_width;
      angle = max_angle * pct;
    } else if (tcx > half_width) {
      float pct = (half_width - tcx) / half_width;
      angle = max_angle * pct;
    }
    (void)angle;
#else
    static float angle = -1.0;
    angle += 1;
    if (angle > 360) {
      angle = 0;
    }
#endif

    BBox tbox = tracking_boxes.at(batch_nr);

    // cudaError_t err_cuda = cudaError_t::cudaSuccess;
    // std::cout << tbox << std::endl;
    // err_cuda = cudaDrawRect(
    //     incoming_surface.dataptr<uchar4*>(),
    //     incoming_surface.dataptr<uchar4*>(),
    //     incoming_surface.pitch() / 4,
    //     incoming_surface.height(),
    //     tbox.left,
    //     tbox.top,
    //     tbox.right,
    //     tbox.bottom,
    //     {0, 0, 0, 0},
    //     /*line_color=*/{255, 255, 0, 255},
    //     /*line_width=*/3.0f,
    //     nppStreamContext.hStream);
    // assert(err_cuda == 0);

    // FloatValue tbox_aar = tbox.width() / tbox.height();

    tbox.left *= scale_w;
    tbox.right *= scale_w;
    tbox.top *= scale_h;
    tbox.bottom *= scale_h;

    // tbox_aar = tbox.width() / tbox.height();

    // err_cuda = cudaDrawRect(
    //     incoming_surface.dataptr<uchar4*>(),
    //     incoming_surface.dataptr<uchar4*>(),
    //     incoming_surface.pitch() / 4,
    //     incoming_surface.height(),
    //     tbox.left,
    //     tbox.top,
    //     tbox.right,
    //     tbox.bottom,
    //     {0, 0, 0, 0},
    //     /*line_color=*/{255, 255, 255, 255},
    //     /*line_width=*/3.0f,
    //     nppStreamContext.hStream);
    // assert(err_cuda == 0);

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
    FloatValue clip_right = std::min(input_rect.right - 1, x_center + min_width_per_side);
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

    // static std::string first_

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
#ifndef NDEBUG
    FloatValue tbox_ar = tbox.width() / tbox.height();
    FloatValue new_tbox_ar = new_tbox.width() / new_tbox.height();
    const BBox output_rect(0, 0, (FloatValue)videoprep->output_width, (FloatValue)videoprep->output_height);
    // FloatValue output_ar = output_rect.width() / output_rect.height();
    assert(isClose(tbox_ar, new_tbox_ar, 1e-6f, 0.001));
    // assert(isClose(new_tbox_ar, output_ar, 1e-6f, 0.001));
#endif

    // We just rotate the whole thing around the point
    // that is effectively the center of the tracking box
#if 0
    {
      auto in_surf_iter = scratch_surface_iter++;
      np_status = rotateNvBufSurfaceWithNPP(
          *in_surf_iter,
          dst_box,
          *scratch_surface_iter,
          dst_box,
          angle,
          /*anchor_point=*/new_tbox.center(),
          nppStreamContext);
      assert(np_status == NppStatus::NPP_SUCCESS);
      // videoprep->priv->render(std::string("Rotate"), *in_surf_iter, nppStreamContext.hStream);
    }
#endif

#if 1
    {
#ifdef __aarch64__
      hm::surface::EglSurfaceMapper outgoinh_elg_surface_mapper(out_surface, batch_nr);
      hm::surface::Surface outgoing_surface = outgoinh_elg_surface_mapper.get_surface();
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
#endif
  if (videoprep->stream) {
    cudaStreamSynchronize(videoprep->stream);
  }

  out_surface->numFilled = nr_surfaces_to_process;

  videoprep::videoprep_add_surface_meta(videoprep->out_gst_buf, nr_surfaces_to_process, videoprep->source_id);
#if 0
  surface_meta->num_filled_surfaces = nr_surfaces_to_process;
  surface_meta->source_id = videoprep->source_id;
  NvDsMeta* meta = NULL;
  meta = gst_buffer_add_nvds_meta(
      videoprep->out_gst_buf, surface_meta, NULL, videoprep_meta_copy_func, videoprep_meta_release_func);

  meta->meta_type = NVDS_DEWARPER_GST_META;
  meta->gst_to_nvds_meta_transform_func = videoprep_gst_to_nvds_meta_ransform_func;
  meta->gst_to_nvds_meta_release_func = videoprep_gst_nvds_meta_release_func;
#endif
  return err;
}

static void gst_stitcher_class_init(GstVideoPrepStitcherClass* klass) {
  gst_videoprep_class_init_base(klass);
}

static void gst_stitcher_init(GstVideoPrepStitcher* stitcher) {
  gst_videoprep_init_base(stitcher);
}

#define gst_stitcher_parent_class parent_class
G_DEFINE_TYPE(GstVideoPrepStitcher, gst_stitcher, GST_TYPE_BASE_TRANSFORM);

} // namespace stitcher
} // namespace hm
