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

#include "cudaMat.h"
#include "showImage.h"

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
      /*batch_size=*/1, /*num_levels=*/0, control_masks, /*match_exposure=*/false);
  if (!stitcher_->status().ok()) {
    return false;
  }

  videoprep->output_width = stitcher_->canvas_width();
  videoprep->output_height = stitcher_->canvas_height();

  return true;
}

struct ModifyBatchFrames {
  ModifyBatchFrames(NvDsBatchMeta* batch_meta, std::vector<NvDsFrameMeta*>& remove) : batch_meta_(batch_meta) {
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

void show_image(std::string label, hm::surface::Surface surface, float scale = 1.0, bool wait = true) {
  hm::CudaMat<uchar4> mat(
      hm::SurfaceInfo{
          .width = (int)surface.width(),
          .height = (int)surface.height(),
          .pitch = (int)surface.pitch(),
          .data_ptr = surface.dataptr(),
      },
      1);
  hm::utils::display_scaled_image(label, mat.download(), scale, wait);
}

cudaError StitcherPriv::GenerateOutput(
    NvDsBatchMeta* batch_meta,
    videoprep::GstVideoPrep* videoprep,
    NvBufSurface* in_surface,
    NvBufSurface* out_surface) {
  // Should not be necessary, debugging some issue atm
  std::unique_lock lk(process_mu_);

  cudaError err = cudaSuccess;
  cudaError_t last_error = cudaGetLastError();
  assert(last_error == cudaSuccess);

  // NvDewarperSurfaceMeta* surface_meta = (NvDewarperSurfaceMeta*)calloc(1, sizeof(NvDewarperSurfaceMeta));

  assert(in_surface->batchSize % 2 == 0);
  assert(in_surface->numFilled % 2 == 0);
  // assert(in_surface->isContiguous);

  struct FrameInfo {
    NvBufSurfaceParams* surface_params;
    // This is the actual frame meta for this surface
    NvDsFrameMeta* frame_meta;
    // This is the frame meta that we'll keep for the final stitched frame (we'll discard on of them)
    NvDsFrameMeta* persistent_frame_meta;
  };

  //  frame_number -> source_id -> NvBufSurfaceParams*
  std::map<int, std::map<int, FrameInfo>> frame_source_surfaces;

  assert(videoprep->stream);

  err = cudaSetDevice(videoprep->gpu_id);
  assert(err == cudaSuccess);

  out_surface->numFilled = 0;
  out_surface->batchSize = in_surface->batchSize / 2;

  std::vector<NvDsFrameMeta*> remove_frame_metas;
  remove_frame_metas.reserve(in_surface->batchSize);

  size_t surface_index = 0;
  for (NvDsFrameMetaList* frame_meta_list = batch_meta->frame_meta_list; frame_meta_list != nullptr;
       frame_meta_list = frame_meta_list->next) {
    assert(frame_meta_list);
    NvDsFrameMeta* frame_meta = (NvDsFrameMeta*)frame_meta_list->data;
    assert(frame_meta->num_surfaces_per_frame == 1);
    auto* surface_params = &in_surface->surfaceList[surface_index];
    // assert(seen_surface_indexes.emplace(frame_meta->surface_index).second);
    std::cout << "source_id=" << frame_meta->source_id << ", frame_num=" << frame_meta->frame_num << std::endl;
    auto& frame_sources = frame_source_surfaces[frame_meta->frame_num];

    NvDsFrameMeta* persistent_frame_meta{nullptr};
    if (!frame_sources.empty()) {
      // We only keep one of the frame meta's for each frame_num, and we'll modify it to
      // mathc the eventual stitched frame.
      // atm, we don't care which source it is, hopefully they come in sorted?
      remove_frame_metas.emplace_back(frame_meta);
      persistent_frame_meta = frame_sources.begin()->second.persistent_frame_meta;
    } else {
      persistent_frame_meta = frame_meta;
    }

#if 0
    if (frame_meta->source_id == 1 && frame_meta->frame_num == 0) {
      show_image("frame", surface_params, 0.2);
    }
#endif
    const bool inserted = frame_sources
                              .emplace(
                                  frame_meta->source_id,
                                  FrameInfo{
                                      .surface_params = surface_params,
                                      .frame_meta = frame_meta,
                                      .persistent_frame_meta = persistent_frame_meta,
                                  })
                              .second;
    assert(inserted);
    ++surface_index;
  }

  // Sanity that the surfaces are laid out as expected
  assert(surface_index == in_surface->numFilled);
  assert(frame_source_surfaces.size() == in_surface->numFilled / 2);
  assert(frame_source_surfaces.begin()->second.size() == frame_source_surfaces.rbegin()->second.size());

  // We will have this many output frames
  const size_t batch_size = frame_source_surfaces.size();
  // out_surface->batchSize = batch_size;
  // out_surface->numFilled = batch_size;
  out_surface->batchSize = batch_size;

  // for (size_t batch_nr = 0; batch_nr < batch_size; batch_nr++) {
  size_t out_surfcace_index = 0;
  for (auto frame_iter = frame_source_surfaces.begin(), frame_end = frame_source_surfaces.end();
       frame_iter != frame_end;
       ++frame_iter, ++out_surfcace_index) {
    const int frame_num = frame_iter->first;
    std::cout << "frame_num=" << frame_num << std::endl;
    auto& source_to_surface = frame_iter->second;
    assert(source_to_surface.size() == 2);

    const FrameInfo& frame_info_left = source_to_surface.begin()->second;
    const FrameInfo& frame_info_right = source_to_surface.rbegin()->second;
    // This tests the assumption that the source ids come in sorted

    const NvBufSurfaceParams* left_params = frame_info_left.surface_params;
    const NvBufSurfaceParams* right_params = frame_info_right.surface_params;
    NvBufSurfaceParams* output_params = &out_surface->surfaceList[out_surfcace_index];

    assert(output_params->width == (uint32_t)stitcher_->canvas_width());
    assert(output_params->height == (uint32_t)stitcher_->canvas_height());
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

    // show_image("left", left_params, 0.2, /*wait=*/false);
    // show_image("right", right_params, 0.2, /*wait=*/false);

    err = cudaMemset(canvas->data(), 128, canvas->width() * canvas->height() * sizeof(uchar4));
    assert(err == cudaError_t::cudaSuccess);

    if (err == cudaError_t::cudaSuccess) {
      auto stitch_result = stitcher_->process(left, right, videoprep->stream, std::move(canvas));
      if (stitch_result.ok()) {
        canvas = std::move(stitch_result.ValueOrDie());
        // render_.render("canvas", output_params, videoprep->stream);
        ++out_surface->numFilled;
        // Both should have the same 'persistent_frame_meta'
        // TODO: Should we do this later under a batch meta lock?
        assert(frame_info_left.persistent_frame_meta == frame_info_right.persistent_frame_meta);
        frame_info_left.persistent_frame_meta->source_frame_width = canvas->width();
        frame_info_left.persistent_frame_meta->source_frame_height = canvas->height();
        frame_info_left.persistent_frame_meta->surface_index = 0; // out_surfcace_index;
        frame_info_left.persistent_frame_meta->pad_index = 0;
        frame_info_left.persistent_frame_meta->pipeline_width = 0;
        frame_info_left.persistent_frame_meta->pipeline_height = 0;
      } else {
        std::cerr << stitch_result.status() << std::endl;
        GST_ERROR("%s\n", stitch_result.status().message().c_str());
      }
    } else {
      std::cerr << "oops" << std::endl;
    }
  }

  if (out_surface->numFilled) {
    ModifyBatchFrames modifier(batch_meta, remove_frame_metas);
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
