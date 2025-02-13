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
#include "nvdsmeta.h"

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
  // I am a little confused about the diufference between these two
  assert(videoprep->num_batch_buffers % 2 == 0);
  videoprep->num_output_buffers = videoprep->num_batch_buffers / 2;

  assert(!stitcher_);
  hm::pano::ControlMasks control_masks;
  if (!control_masks.load(videoprep->config_file)) {
    return false;
  }
  stitcher_ = std::make_unique<hm::pano::cuda::CudaStitchPano<uchar4, float4>>(
      videoprep->num_batch_buffers, /*num+_levels=*/6, control_masks, /*match_exposure=*/false);
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

  // Should not be necessary, debugging some issue atm
  std::unique_lock lk(process_mu_);

  cudaError err = cudaSuccess;

  assert(cudaGetLastError() == cudaSuccess);

  // NvDewarperSurfaceMeta* surface_meta = (NvDewarperSurfaceMeta*)calloc(1, sizeof(NvDewarperSurfaceMeta));

  assert(in_surface->batchSize % 2 == 0);
  assert(in_surface->numFilled % 2 == 0);
  // assert(in_surface->isContiguous);

  //  source_id -> frame_number -> NvBufSurfaceParams*
  std::map<int, std::vector<NvBufSurfaceParams*>> source_frame_surfaces;

  assert(videoprep->stream);

  err = cudaSetDevice(videoprep->gpu_id);
  assert(err == cudaSuccess);

  out_surface->numFilled = 0;

  // TODO: what do we do about this mismatch???
  // NvDsFrameMetaList* frame_meta_list = batch_meta->frame_meta_list;
  // std::unordered_set<int> seen_surface_indexes;
  size_t surface_index = 0;
  for (NvDsFrameMetaList* frame_meta_list = batch_meta->frame_meta_list; frame_meta_list != nullptr;
       frame_meta_list = frame_meta_list->next) {
    assert(frame_meta_list);
    NvDsFrameMeta* frame_meta = (NvDsFrameMeta*)frame_meta_list->data;
    assert(frame_meta->num_surfaces_per_frame == 1);
    // assert(seen_surface_indexes.emplace(frame_meta->surface_index).second);
    source_frame_surfaces[frame_meta->source_id].emplace_back(&in_surface->surfaceList[surface_index]);
    ++surface_index;
  }

  // Sanity that the surfaces are laid out as expected
  assert(surface_index == in_surface->numFilled);
  assert(source_frame_surfaces.size() == 2);
  assert(source_frame_surfaces.begin()->second.size() == source_frame_surfaces.rbegin()->second.size());

  // We will have this many output frames
  const size_t batch_size = source_frame_surfaces.begin()->second.size();
  // out_surface->batchSize = batch_size;
  // out_surface->numFilled = batch_size;
  out_surface->batchSize = batch_size;

  for (size_t batch_nr = 0; batch_nr < batch_size; batch_nr++) {
    NvBufSurfaceParams* left_params = source_frame_surfaces.begin()->second.at(batch_nr);
    NvBufSurfaceParams* right_params = source_frame_surfaces.begin()->second.at(batch_nr);
    NvBufSurfaceParams* output_params = &out_surface->surfaceList[batch_nr];
    assert(output_params->width == stitcher_->canvas_width());
    assert(output_params->height == stitcher_->canvas_height());
    hm::CudaMat<uchar4> left(
        hm::SurfaceInfo{
            .width = (int)left_params->width,
            .height = (int)left_params->height,
            .pitch = (int)left_params->pitch,
            .data_ptr = left_params->dataPtr,
        },
        /*batch_size=*/1);
    hm::CudaMat<uchar4> right(
        hm::SurfaceInfo{
            .width = (int)right_params->width,
            .height = (int)right_params->height,
            .pitch = (int)right_params->pitch,
            .data_ptr = right_params->dataPtr,
        },
        /*batch_size=*/1);
    auto canvas = std::make_unique<hm::CudaMat<uchar4>>(
        hm::SurfaceInfo{
            .width = (int)output_params->width,
            .height = (int)output_params->height,
            .pitch = (int)output_params->pitch,
            .data_ptr = output_params->dataPtr,
        },
        /*batch_size=*/1);

    // render_.render("left", left_params, videoprep->stream);

    err = cudaMemset(canvas->data(), 128, canvas->width() * canvas->height() * sizeof(uchar4));
    assert(err == cudaError_t::cudaSuccess);

    if (err == cudaError_t::cudaSuccess) {
      auto stitch_result = stitcher_->process(left, right, videoprep->stream, std::move(canvas));
      if (stitch_result.ok()) {
        canvas = std::move(stitch_result.ValueOrDie());
        render_.render("canvas", output_params, videoprep->stream);
        ++out_surface->numFilled;
      } else {
        std::cerr << stitch_result.status() << std::endl;
        GST_ERROR("%s\n", stitch_result.status().message().c_str());
      }
    } else {
      std::cerr << "oops" << std::endl;
    }
  }

  if (out_surface->numFilled) {
    cudaStreamSynchronize(videoprep->stream);
  }

  videoprep::videoprep_add_surface_meta(videoprep->out_gst_buf, out_surface->numFilled, videoprep->source_id);
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
