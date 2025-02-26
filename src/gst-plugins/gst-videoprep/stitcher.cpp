#include "gstvideoprep.h"

#include <cuda_runtime.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gstreamer-1.0/gst/gstinfo.h>
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

#if defined(__aarch64__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "cudaEGL.h"
#endif
#include "stitcher.h"

namespace hm {
namespace stitcher {

static constexpr int kNumStitcherLaplacianLevels = 0;
// static constexpr int kNumStitcherLaplacianLevels = 6;

StitcherPriv::~StitcherPriv() {
  stitcher_.reset();
}

bool StitcherPriv::PreCapsInit(DSCustom_CreateParams* params) {
  videoprep::GstVideoPrep* videoprep = GST_VIDEOPREP(params->m_element);
  assert(!stitcher_);
  hm::pano::ControlMasks control_masks;
  if (!control_masks.load(videoprep->config_file)) {
    return false;
  }
  stitcher_ = std::make_unique<hm::pano::cuda::CudaStitchPano<uchar4, float3>>(
      /*batch_size=*/1, /*num_levels=*/kNumStitcherLaplacianLevels, control_masks, /*match_exposure=*/true);
  if (!stitcher_->status().ok()) {
    return false;
  }

  // Not an in-place transform
#ifdef NEW_VIDEOPREP
  m_transformMode = true;
#endif

  m_inVideoFmt = GST_VIDEO_FORMAT_RGBA;
  m_outVideoFmt = GST_VIDEO_FORMAT_RGBA;

  videoprep->output_width = stitcher_->canvas_width();
  videoprep->output_height = stitcher_->canvas_height();

  // videoprep->deref_input_buffer = true;

  g_print("Stitched canvas size: %d x %d\n", (int)stitcher_->canvas_width(), (int)stitcher_->canvas_height());
  return true;
}

bool StitcherPriv::PostCapsInit(DSCustom_CreateParams* params) {
  if (!Super::PostCapsInit(params)) {
    return false;
  }
  return true;
}

bool StitcherPriv::SetProperty(const Property& prop) {
  // std::cerr << "SetProperty(" << prop.key << "=" << prop.value << ")" << std::endl;
  if (prop.key == "left-frame-offset-ns") {
    left_frame_offset_ns_ = std::atol(prop.value.c_str());
  }
  if (prop.key == "right-frame-offset-ns") {
    right_frame_offset_ns_ = std::atol(prop.value.c_str());
  }
  return true;
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

cudaError StitcherPriv::GenerateOutput(
    NvDsBatchMeta* batch_meta,
    videoprep::GstVideoPrep* videoprep,
    NvBufSurface* in_surface,
    NvBufSurface* out_surface) {
  // std::unique_lock lk(process_mu_);

  cudaError err = cudaSuccess;
  cudaError_t last_error = cudaGetLastError();
  assert(last_error == cudaSuccess);

  // NvDewarperSurfaceMeta* surface_meta = (NvDewarperSurfaceMeta*)calloc(1, sizeof(NvDewarperSurfaceMeta));

  assert(in_surface->batchSize % 2 == 0);
  if (in_surface->numFilled % 2 != 0) {
    gst_printerr("Not enough filled surfaces to perform stitching\n");
    return cudaError_t::cudaErrorInvalidValue;
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

  assert(videoprep->stream);

  err = cudaSetDevice(videoprep->gpu_id);
  assert(err == cudaSuccess);

  out_surface->numFilled = 0;
  out_surface->batchSize = in_surface->batchSize / 2;

  std::map<guint, NvDsFrameMeta*> source_frame_metas;

  guint min_source_id = std::numeric_limits<guint>::max();
  guint max_source_id = 0;
  size_t surface_index = 0;
  for (NvDsFrameMetaList* frame_meta_list = batch_meta->frame_meta_list; frame_meta_list != nullptr;
       frame_meta_list = frame_meta_list->next) {
    assert(frame_meta_list);
    NvDsFrameMeta* frame_meta = (NvDsFrameMeta*)frame_meta_list->data;
    assert(frame_meta->num_surfaces_per_frame == 1);
    auto* surface_params = &in_surface->surfaceList[surface_index];
    // assert(seen_surface_indexes.emplace(frame_meta->surface_index).second);
    // std::cout << "source_id=" << frame_meta->source_id << ", frame_num=" << frame_meta->frame_num << std::endl;
    auto& frame_sources = frame_source_surfaces[frame_meta->frame_num];
    min_source_id = std::min(min_source_id, frame_meta->source_id);
    max_source_id = std::max(max_source_id, frame_meta->source_id);

    source_frame_metas.emplace(frame_meta->source_id, frame_meta);

    const bool inserted = frame_sources
                              .emplace(
                                  frame_meta->source_id,
                                  FrameInfo{
                                      .surface_params = surface_params,
                                      .frame_meta = frame_meta,
                                      .incoming_surface_index = surface_index,
                                  })
                              .second;
    assert(inserted);
    ++surface_index;
  }

  // Sanity that the surfaces are laid out as expected
  assert(surface_index == in_surface->numFilled);
  if (frame_source_surfaces.size() != in_surface->numFilled / 2) {
    // This can happen during shutdown
    g_printerr("Stitcher did nto receive the expected source/frame sequence\n");
    return cudaError_t::cudaErrorInvalidSource;
  }
  assert(frame_source_surfaces.size() == in_surface->numFilled / 2);
  assert(frame_source_surfaces.begin()->second.size() == frame_source_surfaces.rbegin()->second.size());

  // We will have this many output frames
  const size_t batch_size = frame_source_surfaces.size();
  out_surface->batchSize = batch_size;

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

    // NvBufSurfaceParams* left_params = frame_info_left.surface_params;
    // NvBufSurfaceParams* right_params = frame_info_right.surface_params;
    NvBufSurfaceParams* output_params = &out_surface->surfaceList[out_surface_index];

    NvDsFrameMeta* reuse_frame_meta{nullptr};
    assert(source_frame_metas.size() == 2);
    if (!left_frame_offset_ns_) {
      // left frame has correct timestamps
      reuse_frame_meta = frame_info_left.frame_meta;
      remove_frame_metas.emplace_back(frame_info_right.frame_meta);
    } else {
      // right frame has correct timestamps
      assert(!right_frame_offset_ns_);
      reuse_frame_meta = frame_info_right.frame_meta;
      remove_frame_metas.emplace_back(frame_info_left.frame_meta);
    }

#ifdef __aarch64__
    hm::surface::EglSurfaceMapper incoming_left_elg_surface_mapper(
        in_surface, frame_info_left.incoming_surface_index, /*read_only=*/true);
    hm::surface::Surface incoming_surface_left = incoming_left_elg_surface_mapper.get_surface();
    hm::surface::EglSurfaceMapper incoming_right_elg_surface_mapper(
        in_surface, frame_info_right.incoming_surface_index, /*read_only=*/true);
    hm::surface::Surface incoming_surface_right = incoming_right_elg_surface_mapper.get_surface();
    hm::surface::EglSurfaceMapper outgoing_elg_surface_mapper(out_surface, out_surface_index, /*read_only=*/false);
    hm::surface::Surface outgoing_surface = outgoing_elg_surface_mapper.get_surface();
#else
    hm::surface::Surface incoming_surface_left(frame_info_left.surface_params);
    hm::surface::Surface incoming_surface_right(frame_info_right.surface_params);
    hm::surface::Surface outgoing_surface(&out_surface->surfaceList[out_surface_index]);
#endif

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

    // render("left", left_params, videoprep->stream);
    // render("right", right_params, videoprep->stream);
    // Why suddenly now I need to clear the canvas?
    cudaMemsetAsync(
        canvas->data_raw(), 0, canvas->height() * canvas->pitch() * canvas->batch_size(), videoprep->stream);

    err = cudaMemsetAsync(canvas->data_raw(), 0, canvas->height() * canvas->pitch() * canvas->batch_size());

    if (err == cudaError_t::cudaSuccess) {
      auto stitch_result = stitcher_->process(left, right, videoprep->stream, std::move(canvas));
      if (stitch_result.ok()) {
        canvas = std::move(stitch_result.ValueOrDie());
        // render("canvas", output_params, videoprep->stream);
        ++out_surface->numFilled;
        // Both should have the same 'persistent_frame_meta'
        // TODO: Should we do this later under a batch meta lock?
        // ModifyBatchFrames frame_adder(batch_meta, remove_frame_metas);
#if 1
        reuse_frame_meta->source_frame_width = reuse_frame_meta->pipeline_width = canvas->width();
        reuse_frame_meta->source_frame_height = reuse_frame_meta->pipeline_height = canvas->height();
        reuse_frame_meta->num_surfaces_per_frame = 1;
#else
        frame_adder.add_frame([&](NvDsFrameMeta* new_frame_meta) -> bool {
          // Currently we don't copy over any user or object meta because it is assumed that thsoe would be wrt an
          // invalid resolution. It may come, however, that we should copy over some stuff, esp user meta,
          // if that ever gets filled.
          assert(!left_frame_offset_ns_ || !right_frame_offset_ns_);
          const FrameInfo* base_frame_info = left_frame_offset_ns_ == 0 ? &frame_info_left : &frame_info_right;

          assert(!base_frame_info->frame_meta->num_obj_meta);
          assert(!base_frame_info->frame_meta->frame_user_meta_list);

          new_frame_meta->frame_num = base_frame_info->frame_meta->frame_num;
          new_frame_meta->source_frame_width = new_frame_meta->pipeline_width = canvas->width();
          new_frame_meta->source_frame_height = new_frame_meta->pipeline_height = canvas->height();
          // new_frame_meta->surface_index = out_surface_index;
          new_frame_meta->pad_index = 0;
          new_frame_meta->source_id = min_source_id;
          new_frame_meta->surface_type = base_frame_info->frame_meta->surface_type;
          new_frame_meta->num_surfaces_per_frame = 1;
          new_frame_meta->batch_id = base_frame_info->frame_meta->batch_id;
          // Use timestamp of the one that is based at 0 so that it matches the audio
          new_frame_meta->ntp_timestamp = base_frame_info->frame_meta->ntp_timestamp;
          assert(min_source_id == 0); // debug checking
          return true;
        });
#endif
      } else {
        std::cerr << stitch_result.status() << std::endl;
        GST_ERROR("%s\n", stitch_result.status().message().c_str());
      }
    } else {
      std::cerr << "oops" << std::endl;
    }
  }

  if (out_surface->numFilled) {
    // batch_meta->num_frames_in_batch /= 2;
    ModifyBatchFrames modifier(batch_meta, remove_frame_metas);
    // batch_meta->max_frames_in_batch /= 2;
    // batch_meta->frame_meta_pool->max_elements_in_pool /= 2;
    batch_meta->max_frames_in_batch = batch_meta->num_frames_in_batch;
    // assert(batch_meta->max_frames_in_batch); // make sure we didnt do too many times and make it 0
    cudaStreamSynchronize(videoprep->stream);
  }
  // videoprep::videoprep_add_surface_meta(videoprep->out_gst_buf, out_surface->numFilled, videoprep->source_id);
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
