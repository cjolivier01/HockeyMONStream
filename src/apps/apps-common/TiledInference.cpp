#include "hstream/src/apps/apps-common/TiledInference.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

#include "gstnvdsmeta.h"
#include "nvbufsurface.h"
#include "yaml-cpp/yaml.h"

namespace hm::pipeline {
namespace {
// Private metadata identity, never exposed past the detector bin.
constexpr guint kTileComponent = G_MAXINT;
struct Context {
  GstElement* infer;
  detection::TileInferenceSettings settings;
};
void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
struct MetaLock {
  NvDsBatchMeta* meta;
  explicit MetaLock(NvDsBatchMeta* meta) : meta(meta) {
    nvds_acquire_meta_lock(meta);
  }
  ~MetaLock() {
    nvds_release_meta_lock(meta);
  }
};
} // namespace
absl::Status PrepareTiledInferenceBuffer(GstBuffer* buffer, const detection::TileInferenceSettings& settings) {
  try {
    Require(gst_buffer_is_writable(buffer), "Tiled inference requires private writable metadata");
    auto* batch = gst_buffer_get_nvds_batch_meta(buffer);
    Require(
        batch && batch->num_frames_in_batch == 1 && batch->frame_meta_list,
        "Tiled inference requires one stitched frame per buffer");
    auto* frame = static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data);
    // NVMM mapping exposes only the surface descriptor. No video pixels are
    // mapped or copied to the CPU; nvinfer owns the batched GPU crop operation.
    GstMapInfo map = GST_MAP_INFO_INIT;
    Require(gst_buffer_map(buffer, &map, GST_MAP_READ), "Cannot read NVMM surface descriptor");
    auto* surface = reinterpret_cast<NvBufSurface*>(map.data);
    const bool valid = map.size >= sizeof(NvBufSurface) && surface->surfaceList && frame->batch_id < surface->numFilled;
    int width = valid ? surface->surfaceList[frame->batch_id].width : 0;
    int height = valid ? surface->surfaceList[frame->batch_id].height : 0;
    gst_buffer_unmap(buffer, &map);
    Require(valid, "Invalid tiled inference surface descriptor");
    const auto tiles = detection::MakeDetectionTiles(width, height, settings);
    if (!tiles.ok())
      throw std::runtime_error(tiles.status().ToString());
    MetaLock lock(batch);
    for (auto* item = frame->obj_meta_list; item; item = item->next)
      Require(
          static_cast<NvDsObjectMeta*>(item->data)->unique_component_id != kTileComponent,
          "Reserved tiled inference component identity already exists");
    for (unsigned index = 0; index < tiles->size(); ++index) {
      const auto& tile = (*tiles)[index];
      Require(tile.width >= 16 && tile.height >= 16, "Canvas is too small for native tile crops");
      auto* object = nvds_acquire_obj_meta_from_pool(batch);
      Require(object, "Cannot allocate tile metadata");
      object->unique_component_id = kTileComponent;
      object->class_id = index;
      object->object_id = UNTRACKED_OBJECT_ID;
      object->rect_params.left = tile.left;
      object->rect_params.top = tile.top;
      object->rect_params.width = tile.width;
      object->rect_params.height = tile.height;
      nvds_add_obj_meta_to_frame(frame, object, nullptr);
    }
    return absl::OkStatus();
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(std::string("Tiled inference: ") + e.what());
  }
}

absl::Status MergeTiledInferenceBuffer(GstBuffer* buffer, const detection::TileInferenceSettings& settings) {
  try {
    auto* batch = gst_buffer_get_nvds_batch_meta(buffer);
    Require(batch && batch->num_frames_in_batch == 1 && batch->frame_meta_list, "Missing tiled inference metadata");
    auto* frame = static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data);
    MetaLock lock(batch);
    std::vector<NvDsObjectMeta*> parents, objects, remove;
    std::vector<detection::TileProposal> proposals;
    for (auto* item = frame->obj_meta_list; item; item = item->next) {
      auto* object = static_cast<NvDsObjectMeta*>(item->data);
      if (object->unique_component_id == kTileComponent) {
        parents.push_back(object);
        continue;
      }
      auto* parent = object->parent;
      if (!parent || parent->unique_component_id != kTileComponent)
        continue;
      auto& r = object->rect_params;
      const auto& tile = parent->rect_params;
      const float right = std::clamp(r.left + r.width, tile.left, tile.left + tile.width);
      const float bottom = std::clamp(r.top + r.height, tile.top, tile.top + tile.height);
      r.left = std::clamp(r.left, tile.left, tile.left + tile.width);
      r.top = std::clamp(r.top, tile.top, tile.top + tile.height);
      r.width = right - r.left;
      r.height = bottom - r.top;
      // nvinfer already restored letterbox scale and added the crop origin.
      // Detach before removing parents: downstream sees ordinary detections.
      object->parent = nullptr;
      if (r.width <= 0 || r.height <= 0) {
        remove.push_back(object);
        continue;
      }
      auto& bbox = object->detector_bbox_info.org_bbox_coords;
      bbox.left = r.left;
      bbox.top = r.top;
      bbox.width = r.width;
      bbox.height = r.height;
      proposals.push_back(
          {{r.left, r.top, right, bottom, object->confidence, object->class_id, static_cast<uint32_t>(objects.size())},
           static_cast<unsigned>(parent->class_id)});
      objects.push_back(object);
    }
    Require(parents.size() == static_cast<size_t>(settings.tiles), "Detector lost its tile regions");
    const auto selected = detection::MergeTileProposals(proposals, settings);
    if (!selected.ok())
      throw std::runtime_error(selected.status().ToString());
    std::vector<bool> keep(objects.size(), false);
    for (size_t index : *selected)
      keep[index] = true;
    for (size_t index = 0; index < objects.size(); ++index)
      if (!keep[index])
        remove.push_back(objects[index]);
    for (auto* object : remove)
      nvds_remove_obj_meta_from_frame(frame, object);
    for (auto* parent : parents)
      nvds_remove_obj_meta_from_frame(frame, parent);
    frame->bInferDone = TRUE;
    return absl::OkStatus();
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(std::string("Tiled inference merge: ") + e.what());
  }
}

absl::Status InstallTiledInference(
    GstElement* infer,
    const char* inference_file,
    const detection::TileInferenceSettings& settings) {
  const auto status = detection::ValidateTileInference(settings);
  if (!status.ok())
    return status;
  try {
    const auto config = YAML::LoadFile(inference_file);
    const auto property = config["property"];
    Require(property.IsMap() && property["network-type"].as<int>() == 0, "Tiling requires an nvinfer detection YAML");
    for (const char* key : {"input-tensor-meta", "output-tensor-meta", "classifier-async-mode", "output-instance-mask"})
      Require(!property[key] || property[key].as<int>() == 0, "Unsupported tiled detector tensor/async/mask option");
    for (const char* key : {"input-object-max-width", "input-object-max-height"})
      Require(!property[key] || property[key].as<int>() == 0, "Tiling does not support ROI maximum-size filters");
    for (const char* key : {"input-object-min-width", "input-object-min-height"})
      Require(!property[key] || property[key].as<int>() <= 16, "Tiling does not support ROI minimum sizes above 16");
    guint batch = 0, interval = 0, unique = 0;
    g_object_get(infer, "batch-size", &batch, "interval", &interval, "unique-id", &unique, nullptr);
    Require(
        batch == static_cast<guint>(settings.tiles) && interval == 0 && unique != kTileComponent,
        "Tiling requires batch-size matching tiles, interval=0 and a nonreserved detector identity");
    // All regions are untracked, so secondary reinference history never
    // suppresses a frame. Keep the original detector ID and role labels.
    g_object_set(
        infer,
        "process-mode",
        2,
        "infer-on-gie-id",
        kTileComponent,
        "infer-on-class-ids",
        settings.tiles == 4 ? "0:1:2:3" : "0:1",
        nullptr);
    for (const char* name : {"sink", "src"}) {
      auto* pad = gst_element_get_static_pad(infer, name);
      Require(pad, "Missing nvinfer pad");
      gst_pad_add_probe(
          pad,
          GST_PAD_PROBE_TYPE_BUFFER,
          [](GstPad* pad, GstPadProbeInfo* info, gpointer data) {
            auto& context = *static_cast<Context*>(data);
            // A tee can still hold the input at a sink-pad probe. Copy only
            // the buffer/metadata header before adding ROIs; GPU memory stays
            // shared and no sibling preview can observe temporary objects.
            auto* buffer = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));
            GST_PAD_PROBE_INFO_DATA(info) = buffer;
            if (!buffer) {
              GST_ELEMENT_ERROR(
                  context.infer, RESOURCE, FAILED, ("Cannot isolate tiled inference metadata"), (nullptr));
              return GST_PAD_PROBE_DROP;
            }
            const auto status = GST_PAD_DIRECTION(pad) == GST_PAD_SINK
                ? PrepareTiledInferenceBuffer(buffer, context.settings)
                : MergeTiledInferenceBuffer(buffer, context.settings);
            if (status.ok())
              return GST_PAD_PROBE_OK;
            GST_ELEMENT_ERROR(context.infer, STREAM, FAILED, ("%s", status.ToString().c_str()), (nullptr));
            return GST_PAD_PROBE_DROP;
          },
          new Context{infer, settings},
          [](gpointer value) { delete static_cast<Context*>(value); });
      gst_object_unref(pad);
    }
    g_print(
        "Tiled inference: %d %s, overlap=%.3f, merge-iou=%.3f, one batch of %d\n",
        settings.tiles,
        settings.tiles == 4 ? "quadrants" : "full-height crops",
        settings.overlap,
        settings.merge_iou,
        settings.tiles);
    return absl::OkStatus();
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(std::string("Tiled inference: ") + e.what());
  }
}
} // namespace hm::pipeline
