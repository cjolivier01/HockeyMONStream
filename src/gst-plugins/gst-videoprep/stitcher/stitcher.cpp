#include "hstream/src/gst-plugins/gst-videoprep/stitcher/stitcher.h"

#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/pipeline_utils.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"

#include "absl/status/status.h"
#include "cupano/cuda/cudaStatus.h"
#include "cupano/pano/cudaMat.h"

// #include "deepstream/sources/includes/nvbufsurface.h"
#include "nvbufsurface.h"

#include <cuda_runtime.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gstreamer-1.0/gst/gstinfo.h>
#include <npp.h>
#include <cmath>
#include "nvdsmeta.h"

#include <assert.h>
#include <cuda.h>
#include <unistd.h>
#include <vector>

#if defined(__aarch64__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "cudaEGL.h"
#endif

namespace hm {
namespace stitcher {

namespace {

std::pair<size_t, size_t> fallback_canvas_from_caps(GstCaps* in_caps) {
  int in_width = 0;
  int in_height = 0;
  if (!in_caps || !getCapsDimensions(in_caps, in_width, in_height)) {
    return {0, 0};
  }
  // Assume side-by-side sources; double the width to keep enough room.
  return {static_cast<size_t>(2 * in_width), static_cast<size_t>(in_height)};
}

void log_canvas_hint(const std::string& prefix, size_t width, size_t height) {
  if (!width || !height) {
    return;
  }
  g_print("%s canvas hint: %zu x %zu\n", prefix.c_str(), width, height);
}

} // namespace

// static constexpr int kNumStitcherLaplacianLevels = 0;
static constexpr int kNumStitcherLaplacianLevels = 11;

static constexpr bool kMatchExposure = false;

StitcherPriv::~StitcherPriv() {
  stitcher_.reset();
}

absl::StatusOr<StitcherPriv::STITCHER*> StitcherPriv::get_stitcher() {
  if (configure_only_ && !one_pass_mode_) {
    return (StitcherPriv::STITCHER*)nullptr;
  }
  if (config_file_.empty()) {
    return absl::NotFoundError("No control masks to load");
  }

  // In one-pass mode we want to be resilient to partial stitcher artifacts (e.g. mapping TIFFs exist but seam_file.png
  // is missing). Without a seam file, hm-cupano will fail to load control masks and we would output a gray canvas.
  if (one_pass_mode_) {
    (void)hm::stitching::maybe_create_default_seam_file(config_file_);
  }

  absl::MutexLock lk(&stitcher_mu_);
  if (!stitcher_) {
    hm::pano::ControlMasks control_masks;
    if (!control_masks.load(config_file_)) {
      std::string config_file_dir = config_file_;
      if (one_pass_mode_) {
        // In one-pass mode, allow the pipeline to bootstrap without masks.
        if (!logged_missing_masks_) {
          g_print("hmstitcher: missing control masks in %s\n", config_file_dir.c_str());
          logged_missing_masks_ = true;
        }
        return (StitcherPriv::STITCHER*)nullptr;
      } else {
        // Don't try again unless one-pass mode wants to re-attempt after configure.
        config_file_.clear();
        return absl::NotFoundError(TO_STRING("Could not load control masks from " << config_file_dir));
      }
    }
    update_canvas_hints(control_masks.canvas_width(), control_masks.canvas_height());
    stitcher_ = std::make_unique<hm::pano::cuda::CudaStitchPano<uchar4, float4>>(
        /*batch_size=*/1, /*num_levels=*/kNumStitcherLaplacianLevels, control_masks, kMatchExposure);
  }
  if (!stitcher_->status().ok()) {
    return to_status(stitcher_->status());
  }
  return stitcher_.get();
}

absl::Status StitcherPriv::reload_stitcher() {
  auto res = get_stitcher();
  if (!res.ok()) {
    return res.status();
  }
  STITCHER* stitcher = res.ok() ? res.value() : nullptr;
  if (stitcher) {
    update_canvas_hints(stitcher->canvas_width(), stitcher->canvas_height());
    log_canvas_hint("hmstitcher", canvas_width_hint_, canvas_height_hint_);
  }
  return absl::OkStatus();
}

absl::Status StitcherPriv::PreCapsInit(DSCustom_CreateParams* params) {
  if (params->config_file) {
    config_file_ = params->config_file;
  }
  auto res = get_stitcher();
  if (!res.ok()) {
    if (one_pass_mode_ && absl::IsNotFound(res.status())) {
      if (!logged_missing_masks_) {
        g_print(
            "hmstitcher: control masks not found in %s; enabling one-pass configure on first batch\n",
            config_file_.c_str());
        logged_missing_masks_ = true;
      }
    } else {
      std::cerr << res.status() << std::endl;
      return res.status();
    }
  }
  STITCHER* stitcher = res.ok() ? res.value() : nullptr;

  // Not an in-place transform
  m_transformMode = true;

  m_inVideoFmt = GST_VIDEO_FORMAT_RGBA;
  m_outVideoFmt = GST_VIDEO_FORMAT_RGBA;

  if (stitcher) {
    // TODO: handle this through caps
    params->output_width_height[0] = stitcher->canvas_width();
    params->output_width_height[1] = stitcher->canvas_height();
    g_print("Stitched canvas size: %d x %d\n", (int)stitcher->canvas_width(), (int)stitcher->canvas_height());
    update_canvas_hints(stitcher->canvas_width(), stitcher->canvas_height());
  } else if (one_pass_mode_) {
    auto fallback = fallback_canvas_from_caps(params->m_inCaps);
    if (fallback.first && fallback.second) {
      params->output_width_height[0] = fallback.first;
      params->output_width_height[1] = fallback.second;
      update_canvas_hints(fallback.first, fallback.second);
      log_canvas_hint("hmstitcher (fallback)", canvas_width_hint_, canvas_height_hint_);
    } else {
      g_print("hmstitcher: no fallback canvas size detected; using passthrough sizing\n");
    }
  }
  return Super::PreCapsInit(params);
}

absl::Status StitcherPriv::PostCapsInit(DSCustom_CreateParams* params) {
  return Super::PostCapsInit(params);
}

bool StitcherPriv::SetProperty(const Property& prop) {
  if (prop.key == "left-frame-offset-ns") {
    left_frame_offset_ns_ = std::atol(prop.value.c_str());
  } else if (prop.key == "right-frame-offset-ns") {
    right_frame_offset_ns_ = std::atol(prop.value.c_str());
  } else if (prop.key == "configure-only") {
    configure_only_ = !!std::atol(prop.value.c_str());
  } else if (prop.key == "one-pass-mode") {
    one_pass_mode_ = !!std::atol(prop.value.c_str());
  } else if (prop.key == "show") {
    show_ = !!std::atol(prop.value.c_str());
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

absl::Status StitcherPriv::GenerateOutput(
    NvDsBatchMeta* batch_meta,
    NvBufSurface* in_surface,
    NvBufSurface* out_surface) {
  assert(in_surface->batchSize % 2 == 0);
  if (in_surface->numFilled % 2 != 0) {
    gst_printerr("Not enough filled surfaces to perform stitching\n");
    return to_status(cudaError_t::cudaErrorInvalidValue);
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

  assert(cuda_stream_);

  HM_RETURN_IF_ERROR(to_status(cudaSetDevice(m_gpuId)));

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
    return to_status(cudaError_t::cudaErrorInvalidSource);
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

    NvBufSurfaceParams* output_params = &out_surface->surfaceList[out_surface_index];

    // render("left", frame_info_left.surface_params, cuda_stream_);
    // render("right", frame_info_right.surface_params, cuda_stream_);

    NvDsFrameMeta* reuse_frame_meta{nullptr};
    assert(source_frame_metas.size() == 2);
    if (!left_frame_offset_ns_) {
      // left frame has correct timestamps
      reuse_frame_meta = frame_info_left.frame_meta;
      remove_frame_metas.emplace_back(frame_info_right.frame_meta);
    } else {
      // Can't get renderer to pick up source-id 1 automatically, even if I change in frame meta

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

    // Maybe configure stitching with these frames
    if (!process_pass_++) {
      bool is_configured;
      HM_ASSIGN_OR_RETURN(is_configured, stitching::is_stitching_configured(config_file_));
      if (!is_configured || configure_only_) {
        if (one_pass_mode_ && !is_configured) {
          g_print("hmstitcher: configuring stitching in one-pass mode\n");
          if (!orientation_ran_) {
            absl::Status orientation_status = stitching::configure_orientation(config_file_);
            if (!orientation_status.ok()) {
              std::cerr << orientation_status << "\n" << std::flush;
              return orientation_status;
            }
            orientation_ran_ = true;
          }
          absl::Status configure_status =
              stitching::configure_stitching(config_file_, incoming_surface_left, incoming_surface_right);
          if (!configure_status.ok()) {
            std::cerr << configure_status << "\n" << std::flush;
            return to_status(CudaStatus(
                cudaError_t::cudaErrorLaunchFailure, (std::stringstream() << configure_status.message()).str()));
          }
          configured_during_run_ = true;
          absl::Status reload_status = reload_stitcher();
          if (!reload_status.ok()) {
            return reload_status;
          }
          if (!stitcher_) {
            return absl::FailedPreconditionError(
                "One-pass stitching configured but control masks could not be loaded");
          }
        } else if (!configure_only_) {
          return absl::FailedPreconditionError("Stitching is not configured");
        } else {
          if (!orientation_ran_) {
            absl::Status orientation_status = stitching::configure_orientation(config_file_);
            if (!orientation_status.ok()) {
              std::cerr << orientation_status << "\n" << std::flush;
              return orientation_status;
            }
            orientation_ran_ = true;
          }
          absl::Status configure_status =
              stitching::configure_stitching(config_file_, incoming_surface_left, incoming_surface_right);
          if (!configure_status.ok()) {
            std::cerr << configure_status << "\n" << std::flush;
            return to_status(CudaStatus(
                cudaError_t::cudaErrorLaunchFailure, (std::stringstream() << configure_status.message()).str()));
          }
          // return absl::CancelledError("Stitching has been configured");
          if (!post_force_pipeline_eos(GST_ELEMENT(m_element))) {
            std::cerr << "Failed to post pipeline EOS, returning an error to stop the pipeline";
            return absl::CancelledError("Stitching has been configured");
          }
        }
      } else if (one_pass_mode_ && !stitcher_) {
        // Masks existed but we had no stitcher due to earlier failure; retry once in one-pass mode.
        absl::Status reload_status = reload_stitcher();
        if (!reload_status.ok()) {
          return reload_status;
        }
        if (configured_during_run_ && !stitcher_) {
          return absl::FailedPreconditionError(
              "One-pass stitching configured but control masks could not be loaded");
        }
      }
    }

    if (canvas_width_hint_ && canvas_height_hint_) {
      if (canvas_width_hint_ > outgoing_surface.width() || canvas_height_hint_ > outgoing_surface.height()) {
        return absl::FailedPreconditionError("Output surface is smaller than expected stitched canvas");
      }
      output_params->width = canvas_width_hint_;
      output_params->height = canvas_height_hint_;
    }

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

    // render("left", left_params, cuda_stream_);
    // render("right", right_params, cuda_stream_);
    // Why suddenly now I need to clear the canvas?

    assert(cuda_stream_);

    if (stitcher_) {
      HM_RETURN_IF_ERROR(to_status(cudaMemsetAsync(
          canvas->data_raw(), 0, canvas->height() * canvas->pitch() * canvas->batch_size(), cuda_stream_)));
      HM_CUDA_ASSIGN_OR_RETURN(canvas, stitcher_->process(left, right, cuda_stream_, std::move(canvas)));
    } else {
      // Gray image
      HM_RETURN_IF_ERROR(to_status(cudaMemsetAsync(
          canvas->data_raw(), 128, canvas->height() * canvas->pitch() * canvas->batch_size(), cuda_stream_)));
    }

    if (one_pass_mode_ && !field_mask_attempted_) {
      field_mask_attempted_ = true;
      bool mask_configured = stitching::is_field_mask_configured(config_file_);
      if (!mask_configured) {
        absl::Status mask_status = stitching::create_field_mask(config_file_, outgoing_surface);
        if (!mask_status.ok()) {
          std::cerr << "Failed to create field mask: " << mask_status << "\n" << std::flush;
        }
      }
    }

    if (show_) {
      render("HM Stitcher (LEFT)", incoming_surface_left, cuda_stream_);
      render("HM Stitcher (RIGHT)", incoming_surface_right, cuda_stream_);
      render("HM Stitcher", outgoing_surface, cuda_stream_);
    }

    // render("canvas", output_params, cuda_stream_);
    ++out_surface->numFilled;
    // Both should have the same 'persistent_frame_meta'
    // TODO: Should we do this later under a batch meta lock?
    // ModifyBatchFrames frame_adder(batch_meta, remove_frame_metas);

    reuse_frame_meta->source_frame_width = reuse_frame_meta->pipeline_width = canvas->width();
    reuse_frame_meta->source_frame_height = reuse_frame_meta->pipeline_height = canvas->height();
    reuse_frame_meta->num_surfaces_per_frame = 1;
    // This transform reduces two input sources (left/right) into a single stitched stream.
    // Downstream elements (nvstreamdemux, perf measurement, sinks) route using `pad_index`/`batch_id`.
    // When we reuse the right frame meta, `pad_index` and `batch_id` would otherwise remain `1` and route the stitched
    // output to source 1 (often unlinked), resulting in a "gray box" / no visible output.
    //
    // Normalize all stitched output frames to the first stream (min pad index) and to the current output surface slot.
    const guint out_pad_index = std::min(frame_info_left.frame_meta->pad_index, frame_info_right.frame_meta->pad_index);
    reuse_frame_meta->pad_index = out_pad_index;
    reuse_frame_meta->batch_id = static_cast<guint>(out_surface_index);
    reuse_frame_meta->surface_index = 0;
    reuse_frame_meta->source_id = min_source_id;
  }

  if (out_surface->numFilled) {
    // batch_meta->num_frames_in_batch /= 2;
    ModifyBatchFrames modifier(batch_meta, remove_frame_metas);
    // batch_meta->max_frames_in_batch /= 2;
    // batch_meta->frame_meta_pool->max_elements_in_pool /= 2;
    batch_meta->max_frames_in_batch = batch_meta->num_frames_in_batch;
    // assert(batch_meta->max_frames_in_batch); // make sure we didnt do too many times and make it 0
    HM_RETURN_IF_ERROR(to_status(cudaStreamSynchronize(cuda_stream_)));
  }
  // videoprep::videoprep_add_surface_meta(videoprep->out_gst_buf, out_surface->numFilled, videoprep->source_id);
  return absl::OkStatus();
}

} // namespace stitcher
} // namespace hm
