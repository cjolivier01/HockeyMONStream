#include "dsfieldmask_lib.h"
#include "fieldmask_payload.h"
#include "hstream/src/gst-plugins/gst-fieldmask/fieldmask_payload.h"
#include "hstream/src/libs/common/PlotContext.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

#include "absl/status/status.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <memory>
#include <optional>

#include <opencv2/core/types.hpp>
#include <stdio.h>
#include <stdlib.h>

#include "nvbufsurface.h"

namespace fs = std::filesystem;

#ifdef HAS_NVDS_CUSTOMUSERMETA
using FieldMaskPayload = hm::fieldmask::FieldMaskPayload;
#endif

struct DsFieldMaskCtx {
  DsFieldMaskInitParams initParams;
  hm::fieldmask::IceBoundaryOffsets offsets;
  hm::fieldmask::RinkMaskInsets mask_insets;
  bool mask_insets_dirty{true};
  cv::Mat exclusion_mask;
  size_t total_frame_count{0};
  cv::Mat detection_bit_mask;
  cv::Mat detection_u8_mask;
  cv::Point2f detection_mask_centroid;
  cv::Rect2i field_box;
  bool logged_mask_size_mismatch{false};
  std::string loaded_output_generation;
  std::string loaded_output_authorization_id;
  std::optional<std::string> superseded_output_generation;
  std::string superseded_output_authorization_id;
  std::unique_ptr<hm::stitching::FieldMaskPublicationAuthorityMonitor> superseded_authority_monitor;
  std::string calibration_invalidation_id;
  std::optional<fs::file_time_type> required_mask_mtime;
  uintmax_t required_mask_size{0};
};

namespace {

bool is_bit_set(const cv::Mat& mask, const cv::Point& point) {
  int byteIndex = (point.y * mask.cols + point.x / 8); // Byte index in the data
  int bitIndex = point.x % 8; // Bit index within the byte
  // int bitIndex = point.x & 7;
  return (mask.data[byteIndex] & (1 << (7 - bitIndex))) != 0;
  // int bitIndex = point.x % 8; // Bit index within the byte
  // uchar val = mask.at<uchar>(cv::Point2l(point.x/8, point.y));
  // return (val & (1 << bitIndex)) != 0;
}

// Convert 8-bit-per-pixel mask to 1-bit-per-pixel (packed as bytes)
cv::Mat convert_to_bit_mask(const cv::Mat& inputMask) {
  if (inputMask.type() != CV_8UC1) {
    throw std::invalid_argument("Input mask must be a CV_8UC1 matrix.");
  }

  // Output matrix: 1/8th the number of columns (rounded up), same rows
  int packedCols = (inputMask.cols + 7) / 8;
  cv::Mat bitMask(inputMask.rows, packedCols, CV_8UC1, cv::Scalar(0));

  for (int y = 0; y < inputMask.rows; ++y) {
    const uchar* inputRow = inputMask.ptr<uchar>(y);
    uchar* outputRow = bitMask.ptr<uchar>(y);

    for (int x = 0; x < inputMask.cols; ++x) {
      if (inputRow[x] != 0) {
        outputRow[x / 8] |= (1 << (7 - (x % 8))); // Set the corresponding bit
      }
    }
  }

  return bitMask;
}

// Compute the centroid of a binary mask
cv::Point2f compute_centroid(const cv::Mat& mask, cv::Rect2i& bbox) {
  if (mask.type() != CV_8UC1) {
    throw std::invalid_argument("Input mask must be a CV_8UC1 matrix.");
  }
  // cv::Rect bbox(0,)
  double sumX = 0.0, sumY = 0.0;
  int count = 0;

  int low_x = std::numeric_limits<int>::max(), low_y = std::numeric_limits<int>::max();
  int high_x = -1, high_y = -1;

  for (int y = 0; y < mask.rows; ++y) {
    const uchar* row = mask.ptr<uchar>(y);
    for (int x = 0; x < mask.cols; ++x) {
      if (row[x] != 0) { // Check for non-zero values
        sumX += x;
        sumY += y;
        low_x = std::min(x, low_x);
        low_y = std::min(y, low_y);
        high_x = std::max(x, high_x);
        high_y = std::max(y, high_y);
        ++count;
      }
    }
  }

  if (count == 0) {
    throw std::runtime_error("The mask has no non-zero pixels.");
  }

  if (low_x != std::numeric_limits<int>::max() && low_y != std::numeric_limits<int>::max() && high_x > 0 &&
      high_y > 0) {
    bbox = cv::Rect2i(low_x, low_y, high_x - low_x, high_y - low_y);
  } else {
    bbox = cv::Rect2i(0, 0, 0, 0);
  }

  // Return the centroid as a floating-point point
  return cv::Point2f(sumX / count, sumY / count);
}

void prune_detection_boxes(NvDsFrameMeta* frame_meta, const DsFieldMaskCtx* ctx, bool draw) {
  if (!frame_meta->obj_meta_list || !frame_meta->bInferDone) {
    return;
  }

  std::unique_ptr<hm::utils::PlotContext> plot_context;

  if (guint(ctx->detection_u8_mask.cols) != frame_meta->source_frame_width ||
      guint(ctx->detection_u8_mask.rows) != frame_meta->source_frame_height) {
    auto* ctx_mut = const_cast<DsFieldMaskCtx*>(ctx);
    if (!ctx_mut->logged_mask_size_mismatch) {
      ctx_mut->logged_mask_size_mismatch = true;
      g_printerr(
          "ds-fieldmask: detection mask size %dx%d does not match frame %ux%u; skipping prune\n",
          ctx->detection_u8_mask.cols,
          ctx->detection_u8_mask.rows,
          frame_meta->source_frame_width,
          frame_meta->source_frame_height);
    }
    return;
  }

  assert(frame_meta->pipeline_height);
  assert(frame_meta->pipeline_width);

  const float scale_height = float(frame_meta->source_frame_height) / frame_meta->pipeline_height;
  const float scale_width = float(frame_meta->source_frame_width) / frame_meta->pipeline_width;

  NvDsMetaList* l_next = nullptr;
  // static float max_y = 0;
  // static float max_x = 0;
  for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_next) {
    l_next = l_obj->next;

    if (draw && !plot_context) {
      plot_context = std::make_unique<hm::utils::PlotContext>(frame_meta);
    }

    NvDsMetaList* remove_me{nullptr};
    NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
    NvBbox_Coords bbox_coords = obj_meta->detector_bbox_info.org_bbox_coords;
    if (bbox_coords.width <= 0.0F || bbox_coords.height <= 0.0F) {
      bbox_coords.left = obj_meta->rect_params.left;
      bbox_coords.top = obj_meta->rect_params.top;
      bbox_coords.width = obj_meta->rect_params.width;
      bbox_coords.height = obj_meta->rect_params.height;
    }
    const auto sample = hm::fieldmask::ice_boundary_sample(
        bbox_coords.left + bbox_coords.width * 0.5F,
        bbox_coords.top + bbox_coords.height,
        bbox_coords.width,
        bbox_coords.height,
        {ctx->detection_mask_centroid.x / scale_width, ctx->detection_mask_centroid.y / scale_height},
        ctx->offsets);
    const cv::Point2f point(
        std::clamp(sample.x * scale_width, 0.0F, float(frame_meta->source_frame_width - 1)),
        std::clamp(sample.y * scale_height, 0.0F, float(frame_meta->source_frame_height - 1)));
    if (plot_context)
      plot_context->plot_circle({sample.x, sample.y}, 3, 2, hm::utils::ColorRGB{255, 0, 0});
    if (!is_bit_set(ctx->detection_bit_mask, point))
      remove_me = l_obj;
    if (remove_me) {
      nvds_remove_obj_meta_from_frame(frame_meta, obj_meta);
    }
  }
}
} // namespace

DsFieldMaskCtx* DsFieldMaskCtxInit(DsFieldMaskInitParams* initParams) {
  DsFieldMaskCtx* ctx = new DsFieldMaskCtx();
  ctx->initParams = *initParams;
  ctx->mask_insets = initParams->mask_insets;
  ctx->offsets = {
      initParams->raise_bbox_center_by_height_ratio,
      initParams->lower_bbox_bottom_by_height_ratio,
      initParams->left_bbox_by_half_width_ratio,
      initParams->right_bbox_by_half_width_ratio};
  const char* calibration_invalidation_id = g_getenv("HSTREAM_CALIBRATION_INVALIDATION_ID");
  ctx->calibration_invalidation_id = calibration_invalidation_id ? calibration_invalidation_id : "";
  return ctx;
}

void DsFieldMaskSetOffsets(DsFieldMaskCtx* ctx, const hm::fieldmask::IceBoundaryOffsets& offsets) {
  ctx->offsets = offsets;
}

void DsFieldMaskSetInsets(DsFieldMaskCtx* ctx, const hm::fieldmask::RinkMaskInsets& insets) {
  if (!(ctx->mask_insets == insets)) {
    ctx->mask_insets = insets;
    ctx->mask_insets_dirty = true;
  }
}

absl::Status DsFieldMaskProcessFrame(
    NvBufSurface* surface,
    size_t frame_index,
    NvDsFrameMeta* frame_meta,
    DsFieldMaskCtx* ctx,
    bool draw) {
  if (ctx->initParams.detection_mask_file.empty()) {
    if (ctx->initParams.require_existing_mask)
      return absl::FailedPreconditionError("Analysis requires a configured existing rink mask");
    // We are a No-op
    return absl::OkStatus();
  }
  if (!frame_meta)
    return absl::InvalidArgumentError("Field-mask processing requires frame metadata");

  std::string output_generation;
  std::string output_authorization_id;
  std::string loaded_hugin_generation;
  if (const auto* payload = hm::stitching::find_stitched_output_generation_meta(frame_meta)) {
    output_generation = payload->generation();
    output_authorization_id = payload->authorization_id();
    loaded_hugin_generation = payload->hugin_generation();
  }
  if (ctx->total_frame_count > 0 && !ctx->loaded_output_generation.empty() && output_generation.empty()) {
    return absl::FailedPreconditionError("Stitched-output generation metadata disappeared after mask loading");
  }

  if (ctx->initParams.require_existing_mask) {
    if (output_generation.empty() || loaded_hugin_generation.empty())
      return absl::FailedPreconditionError("Analysis requires stitched-output generation metadata for rink pruning");
    const fs::path mask_path(ctx->initParams.detection_mask_file);
    std::error_code error;
    if (!fs::is_regular_file(mask_path, error) ||
        !fs::equivalent(mask_path, mask_path.parent_path() / "rink_mask_0.png", error))
      return absl::FailedPreconditionError("Analysis requires the existing generation-bound rink_mask_0.png");
    // The scan caches immutable mask pixels. Stat and authority checks catch
    // replacement without repeated PNG decode/packing on large native canvases.
    // The scan owner also binds the exact content hash at startup/completion.
    const auto mask_mtime = fs::last_write_time(mask_path, error);
    if (error)
      return absl::FailedPreconditionError("Could not inspect required rink mask: " + error.message());
    const auto mask_size = fs::file_size(mask_path, error);
    if (error)
      return absl::FailedPreconditionError("Could not inspect required rink mask: " + error.message());
    if (ctx->required_mask_mtime && (*ctx->required_mask_mtime != mask_mtime || ctx->required_mask_size != mask_size))
      return absl::FailedPreconditionError("Required rink mask changed during analysis");
    ctx->required_mask_mtime = mask_mtime;
    ctx->required_mask_size = mask_size;
    HM_RETURN_IF_ERROR(
        hm::stitching::validate_field_mask_publication_authority(
            mask_path.parent_path().string(), output_generation, output_authorization_id));
  }

  // Only consider the mask "obsolete" if we've already loaded one but it no longer matches the current frame size.
  // On first frame, `detection_u8_mask` is empty and has (cols, rows) = (0, 0); treating that as obsolete would
  // unnecessarily regenerate the rink mask and block the whole pipeline.
  bool is_obsolete_detection_mask = false; // TODO: only check first frame
  if (!ctx->detection_u8_mask.empty() &&
      (guint(ctx->detection_u8_mask.cols) != frame_meta->source_frame_width ||
       guint(ctx->detection_u8_mask.rows) != frame_meta->source_frame_height)) {
    is_obsolete_detection_mask = true;
  }

  const bool output_generation_changed = output_generation != ctx->loaded_output_generation ||
      output_authorization_id != ctx->loaded_output_authorization_id;
  if (ctx->initParams.require_existing_mask &&
      (is_obsolete_detection_mask || (ctx->total_frame_count > 0 && output_generation_changed)))
    return absl::FailedPreconditionError(
        "Analysis stitched-output generation or dimensions changed after mask loading");
  if (ctx->superseded_output_generation.has_value() &&
      (*ctx->superseded_output_generation != output_generation ||
       ctx->superseded_output_authorization_id != output_authorization_id)) {
    ctx->superseded_output_generation.reset();
    ctx->superseded_output_authorization_id.clear();
    ctx->superseded_authority_monitor.reset();
  }
  if (ctx->detection_u8_mask.empty() || is_obsolete_detection_mask || output_generation_changed) {
    fs::path mask_path = ctx->initParams.detection_mask_file;
    const auto load_current_mask = [&]() {
      if (!output_generation.empty() && !loaded_hugin_generation.empty()) {
        return hm::stitching::load_field_mask_for_loaded_generation(
            mask_path.parent_path().string(), output_generation, loaded_hugin_generation);
      }
      return hm::stitching::load_field_mask(mask_path.parent_path().string(), output_generation);
    };
    if (ctx->superseded_output_generation.has_value()) {
      if (!ctx->superseded_authority_monitor)
        return absl::InternalError("Publication authority monitor is unavailable");
      const absl::Status authority = ctx->superseded_authority_monitor->status();
      if (absl::IsUnavailable(authority))
        return absl::OkStatus();
      HM_RETURN_IF_ERROR(authority);
      ctx->superseded_output_generation.reset();
      ctx->superseded_output_authorization_id.clear();
      ctx->superseded_authority_monitor.reset();
    }
    auto loaded_mask = load_current_mask();
    if (is_obsolete_detection_mask || !loaded_mask.ok()) {
      if (ctx->initParams.require_existing_mask)
        return loaded_mask.ok() ? absl::FailedPreconditionError("Required rink mask has obsolete dimensions")
                                : loaded_mask.status();
      if (!surface) {
        return absl::FailedPreconditionError("Cannot create field mask without an input surface");
      }
      assert(frame_index < surface->numFilled);
#ifdef __aarch64__
      hm::surface::EglSurfaceMapper egl_surface_mapper(surface, frame_index, /*read_only=*/true);
      HM_RETURN_IF_ERROR(hm::to_status(egl_surface_mapper.status()));
      hm::surface::Surface this_surface = egl_surface_mapper.get_surface();
#else
      hm::surface::Surface this_surface(&surface->surfaceList[frame_index]);
#endif
      const absl::Status created = hm::stitching::create_field_mask(
          mask_path.parent_path().string(),
          this_surface,
          output_generation,
          ctx->calibration_invalidation_id,
          {},
          output_authorization_id);
      if (absl::IsAborted(created)) {
        ctx->superseded_output_generation = output_generation;
        ctx->superseded_output_authorization_id = output_authorization_id;
        if (!ctx->superseded_authority_monitor)
          ctx->superseded_authority_monitor = std::make_unique<hm::stitching::FieldMaskPublicationAuthorityMonitor>();
        HM_RETURN_IF_ERROR(ctx->superseded_authority_monitor->Watch(
            mask_path.parent_path().string(), output_generation, output_authorization_id));
        return absl::OkStatus();
      }
      HM_RETURN_IF_ERROR(created);
      loaded_mask = load_current_mask();
    }
    HM_ASSIGN_OR_RETURN(ctx->detection_u8_mask, std::move(loaded_mask));
    if (ctx->initParams.require_existing_mask &&
        (ctx->detection_u8_mask.empty() || ctx->detection_u8_mask.type() != CV_8UC1 ||
         cv::countNonZero(ctx->detection_u8_mask) == 0))
      return absl::FailedPreconditionError("Analysis requires a nonempty CV_8UC1 rink mask with ice pixels");
    ctx->detection_mask_centroid = compute_centroid(ctx->detection_u8_mask, ctx->field_box);
    ctx->mask_insets_dirty = true;
    ctx->loaded_output_generation = output_generation;
    ctx->loaded_output_authorization_id = output_authorization_id;
  }
  if (ctx->initParams.require_existing_mask &&
      (static_cast<guint>(ctx->detection_u8_mask.cols) != frame_meta->source_frame_width ||
       static_cast<guint>(ctx->detection_u8_mask.rows) != frame_meta->source_frame_height ||
       frame_meta->pipeline_width != frame_meta->source_frame_width ||
       frame_meta->pipeline_height != frame_meta->source_frame_height))
    return absl::FailedPreconditionError("Analysis rink-mask dimensions do not match the stitched frame");
  if (ctx->mask_insets_dirty) {
    try {
      ctx->exclusion_mask = hm::fieldmask::inset_rink_mask(ctx->detection_u8_mask, ctx->mask_insets);
      ctx->detection_bit_mask = convert_to_bit_mask(ctx->exclusion_mask);
    } catch (const std::exception& error) {
      return absl::InternalError(std::string("Could not adjust the rink exclusion mask: ") + error.what());
    }
    ctx->mask_insets_dirty = false;
  }
  prune_detection_boxes(frame_meta, ctx, draw);
#ifdef HAS_NVDS_CUSTOMUSERMETA
  if (frame_meta && frame_meta->base_meta.batch_meta) {
    FieldMaskPayload::create_and_add<FieldMaskPayload>(
        frame_meta,
        ctx->detection_mask_centroid,
        ctx->field_box,
        ctx->detection_u8_mask,
        ctx->loaded_output_generation + ":" + ctx->loaded_output_authorization_id,
        ctx->offsets,
        ctx->exclusion_mask);
  }
#endif
  ++ctx->total_frame_count;
  return absl::OkStatus();
}

void DsFieldMaskCtxDeinit(DsFieldMaskCtx* ctx) {
  if (ctx) {
    delete ctx;
  }
}
