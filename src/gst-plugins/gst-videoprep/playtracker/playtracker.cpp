#include "hstream/src/gst-plugins/gst-videoprep/playtracker/playtracker.h"
#include <cuda_runtime.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <npp.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include "absl/status/status.h"
#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "hstream/src/gst-plugins/gst-videoprep/playtracker/playtracker_payload.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/TempFile.h"
#include "hstream/src/libs/draw_display/DrawDisplayMeta.h"
#include "nvbufsurface.h"
#include "nvdsmeta.h"

#include <assert.h>
#include <cuda.h>
#include <unistd.h>
#include <yaml-cpp/node/parse.h>

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

  YAML::Node config = YAML::LoadFile(init_params_.play_tracker_config_file);
  std::cout << config << std::endl;
  std::vector<YAML::Node> live_boxes;
  for (YAML::Node box : config["play-tracker"]["live-boxes"]) {
    box["arena-angle-from-vertical"] = std::to_string(fixed_edge_rotation_angle_);
    live_boxes.push_back(box);
  }
  if (!live_boxes.empty()) {
    (*live_boxes.rbegin())["dynamic-acceleration-scaling"] = std::to_string(dynamic_acceleration_scaling_);
  }

  // std::cout << config << std::endl;

  auto temp_yaml_file = std::make_unique<hm::utils::TempFile>(/*autoRemove=*/true);
  // std::cout << "Temporary play tracker conrfig file: " << temp_yaml_file->getPath() << std::endl;
  std::ofstream ofile(temp_yaml_file->getPath());
  ofile << config;
  ofile.close();
  init_params_.play_tracker_config_file = temp_yaml_file->getPath();
  init_params_.owned_objects.emplace_back(std::move(temp_yaml_file));
  pt_context_ = DsPlayTrackerCtxInit(&init_params_);
  return Super::PostCapsInit(params);
}

bool PlayTrackerPriv::SetProperty(const Property& prop) {
  if (prop.key == "show") {
    show_ = !!std::atol(prop.value.c_str());
  } else if (prop.key == "draw") {
    init_params_.draw = !!std::atol(prop.value.c_str());
  } else if (prop.key == "fixed-edge-rotation-angle") {
    fixed_edge_rotation_angle_ = std::atof(prop.value.c_str());
  } else if (prop.key == "dynamic-acceleration-scaling") {
    dynamic_acceleration_scaling_ = std::atof(prop.value.c_str());
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
    assert(frame.batch_index < in_surface->numFilled);
    frame.frame_meta = (NvDsFrameMeta*)fl->data;
    frame.input_surf_params = &in_surface->surfaceList[frame.batch_index];
    if (frame_counter_ % frame_calculation_interval_ == 0) {
      if (!DsPlayTrackerProcessFrame(pt_context_, frame, cuda_stream_)) {
        return absl::InternalError("Error calling DsPlayTrackerProcessFrame()");
      }
#ifdef HAS_USER_APPLICATION_PAYLOAD
      PlayTrackerPayload::create_and_add<PlayTrackerPayload>(frame.frame_meta, pt_context_->arena_box);
#endif
      prev_play_tracker_results_ = frame.play_tracker_results;
    } else {
      assert(false);
      frame.play_tracker_results = prev_play_tracker_results_;
      if (pt_context_->initParams.draw) {
        HM_RETURN_IF_ERROR(DsPlayTrackerDrawToDisplayMeta(pt_context_, frame));
      }
      DsPlayTrackerAttachMetadataFullFrame(frame.frame_meta, frame.play_tracker_results);
    }
    if (show_) {
      NvDisplayMetaList* dm_list = frame.frame_meta->display_meta_list;
      while (dm_list) {
        NvDsDisplayMeta* display_meta = (NvDsDisplayMeta*)dm_list->data;
        HM_RETURN_IF_ERROR(draw_display_meta(frame.input_surf_params, display_meta, font_cache, 1.0f, cuda_stream_));
        dm_list = dm_list->next;
      }
    }
    ++frame.batch_index;
    ++frame_counter_;
    fl = fl->next;
  }
  return absl::OkStatus();
}

} // namespace playtracker
} // namespace hm
