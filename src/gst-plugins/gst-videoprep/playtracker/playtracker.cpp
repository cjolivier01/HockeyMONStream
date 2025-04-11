#include "playtracker.h"
#include "playtracker_payload.h"

#include <absl/status/status.h>
#include <cuda_runtime.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <npp.h>
#include <cmath>
#include "deepstream/sources/includes/nvbufsurface.h"
#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/draw_display/DrawDisplayMeta.h"
#include "nvdsmeta.h"

#include <assert.h>
#include <cuda.h>
#include <unistd.h>

namespace hm {
namespace playtracker {

PlayTrackerPriv::~PlayTrackerPriv() {
  if (pt_context_) {
    DsPlayTrackerCtxDeinit(pt_context_);
    pt_context_ = nullptr;
  }
}

absl::Status PlayTrackerPriv::PreCapsInit(DSCustom_CreateParams* params) {
  return Super::PreCapsInit(params);
};

absl::Status PlayTrackerPriv::PostCapsInit(DSCustom_CreateParams* params) {
  // Transform In-Place (but still asynchronously)
  m_transformMode = false;
  // No buffers for us
  params->m_bufferPoolConfig.max_buffers = 0;
  if (params->config_file && init_params_.play_tracker_config_file.empty()) {
    init_params_.play_tracker_config_file = params->config_file;
  }
  pt_context_ = DsPlayTrackerCtxInit(&init_params_);
  return Super::PostCapsInit(params);
}

bool PlayTrackerPriv::SetProperty(const Property& prop) {
  if (prop.key == "show") {
    show_ = !!std::atol(prop.value.c_str());
  } else if (prop.key == "draw") {
    init_params_.draw = !!std::atol(prop.value.c_str());
  } else if (prop.key == "config-file") {
    init_params_.play_tracker_config_file = prop.value;
  }
  return true;
}

BufferResult PlayTrackerPriv::ProcessBuffer(GstBuffer* inbuf) {
  return Super::ProcessBuffer(inbuf);
}

absl::Status PlayTrackerPriv::GenerateOutput(
    NvDsBatchMeta* batch_meta,
    NvBufSurface* in_surface,
    NvBufSurface* /*out_surface*/) {
  GstDsPlayTrackerFrame frame;
  auto font_cache = draw_display::get_or_create_font_cache();
  NvDsFrameMetaList* fl = batch_meta->frame_meta_list;
  while (fl) {
    if (frame_counter_ % frame_calculation_interval_ == 0) {
      assert(frame.batch_index < in_surface->numFilled);
      frame.frame_meta = (NvDsFrameMeta*)fl->data;
      frame.input_surf_params = &in_surface->surfaceList[frame.batch_index];
      if (!DsPlayTrackerProcessFrame(pt_context_, frame, cuda_stream_)) {
        return absl::InternalError("Error calling DsPlayTrackerProcessFrame()");
      }
      PlayTrackerPayload::create_and_add<PlayTrackerPayload>(frame.frame_meta, pt_context_->arena_box);
      if (show_) {
        NvDisplayMetaList* dm_list = frame.frame_meta->display_meta_list;
        while (dm_list) {
          NvDsDisplayMeta* display_meta = (NvDsDisplayMeta*)dm_list->data;
          HM_RETURN_IF_ERROR(draw_display_meta(frame.input_surf_params, display_meta, font_cache, 1.0f, cuda_stream_));
          dm_list = dm_list->next;
        }
      }
      prev_play_tracker_results_ = frame.play_tracker_results;
    } else {
      frame.play_tracker_results = prev_play_tracker_results_;
    }
    ++frame.batch_index;
    ++frame_counter_;
    fl = fl->next;
  }
  return absl::OkStatus();
}

} // namespace playtracker
} // namespace hm
