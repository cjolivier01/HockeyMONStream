#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "hstream/src/gst-plugins/gst-videoprep/playcropper/playcropper.h"
#include "hstream/src/libs/player_analytics/FrameMeta.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
namespace pa = hm::player_analytics;
namespace overlay = hm::draw_display::analytics;
namespace preview = hm::preview_overlay;

void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void Cuda(cudaError_t value) {
  if (value != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(value));
}

// Offline fixture upload/readback only. Production GenerateOutput receives
// ordinary GPU surfaces; this test never substitutes its transform or renderer.
class Surface {
 public:
  Surface(unsigned width, unsigned height) : width(width), height(height) {
    NvBufSurfaceCreateParams params{};
    params.width = width;
    params.height = height;
    params.gpuId = 0;
    params.layout = NVBUF_LAYOUT_PITCH;
    params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
#if defined(__aarch64__)
    params.memType = NVBUF_MEM_SURFACE_ARRAY;
#else
    params.memType = NVBUF_MEM_CUDA_DEVICE;
#endif
    Check(NvBufSurfaceCreate(&value, 1, &params) == 0, "NvBufSurface fixture allocation failed");
    value->numFilled = 1;
  }
  ~Surface() {
    NvBufSurfaceDestroy(value);
  }
  std::vector<unsigned char> Pixels(const std::vector<unsigned char>* upload = nullptr) {
#if defined(__aarch64__)
    hm::surface::EglSurfaceMapper mapper(value, 0, upload == nullptr);
    Cuda(mapper.status());
    auto view = mapper.get_surface();
#else
    hm::surface::Surface view(&value->surfaceList[0]);
#endif
    std::vector<unsigned char> pixels(width * height * 4);
    if (upload) {
      Check(upload->size() == pixels.size(), "invalid upload size");
      Cuda(cudaMemcpy2D(
          view.dataptr(), view.pitch(), upload->data(), width * 4, width * 4, height, cudaMemcpyHostToDevice));
    } else {
      Cuda(cudaMemcpy2D(
          pixels.data(), width * 4, view.dataptr(), view.pitch(), width * 4, height, cudaMemcpyDeviceToHost));
    }
    Cuda(cudaDeviceSynchronize()); // Fixture imports retire after upload/readback.
    return pixels;
  }
  NvBufSurface* value{nullptr};
  unsigned width, height;
};

class Cropper : public hm::playcropper::PlayCropperPriv {
 public:
  explicit Cropper(cudaStream_t stream) : PlayCropperPriv(0, 1) {
    cuda_stream_ = stream;
    m_buffer_pool_config.max_buffers = 1;
    Check(SetProperty({"fixed-edge-rotation-angle-left", "17"}), "left rotation rejected");
    Check(SetProperty({"fixed-edge-rotation-angle-right", "9"}), "right rotation rejected");
    Check(SetProperty({"transform-object-meta", "1"}), "metadata transform rejected");
  }
  bool allocated() const {
    return player_overlay_compositor_ != nullptr;
  }
  void Suppress() {
    overlay::Limits limits;
    limits.upload_bytes = 1;
    player_overlay_compositor_ = std::make_unique<overlay::Compositor>(limits);
  }
  uint64_t suppressions() const {
    return player_overlay_suppressions_;
  }
};

class Metadata {
 public:
  Metadata() {
    source = nvds_create_batch_meta(1);
    program = nvds_create_batch_meta(1);
    Check(source && program, "batch metadata allocation failed");
    original = nvds_acquire_frame_meta_from_pool(source);
    Check(original, "source frame allocation failed");
    original->source_id = 3;
    original->pad_index = 3;
    original->buf_pts = 1230000000;
    original->frame_num = 17;
    original->source_frame_width = 640;
    original->source_frame_height = 480;
    nvds_add_frame_meta_to_batch(source, original);
    auto* player = nvds_acquire_obj_meta_from_pool(source);
    Check(player, "person allocation failed");
    player->class_id = 0;
    player->object_id = (uint64_t{1} << 40) + 27;
    player->rect_params = {};
    player->rect_params.left = 240;
    player->rect_params.top = 160;
    player->rect_params.width = 80;
    player->rect_params.height = 120;
    nvds_add_obj_meta_to_frame(original, player, nullptr);
    auto* camera = nvds_acquire_obj_meta_from_pool(source);
    Check(camera, "camera box allocation failed");
    camera->class_id = DsPlayTrackerInitParams::kPlayBoxClassIdBase;
    camera->object_id = UNTRACKED_OBJECT_ID;
    camera->rect_params = {};
    camera->rect_params.left = 80;
    camera->rect_params.top = 60;
    camera->rect_params.width = 360;
    camera->rect_params.height = 300;
    nvds_add_obj_meta_to_frame(original, camera, nullptr);
    pa::FrameResult result;
    result.stream_id = 3;
    result.epoch = 2;
    result.sequence = 17;
    result.pts_ns = original->buf_pts;
    result.coordinate_width = 640;
    result.coordinate_height = 480;
    result.player_count = 1;
    auto& semantic = result.players[0];
    semantic.track_id = player->object_id;
    semantic.box = {240, 160, 80, 120};
    semantic.has_pose = true;
    semantic.pose_observed_at = result.pts_ns;
    semantic.color_slot = 2;
    // One confident joint isolates the caller's exact crop/rotation/resize map.
    semantic.pose[0] = {280, 220, 0.9F};
    Check(pa::AttachFrameResult(original, result), "semantic source attachment failed");
    output = nvds_acquire_frame_meta_from_pool(program);
    Check(output, "Program frame allocation failed");
    nvds_add_frame_meta_to_batch(program, output);
    nvds_copy_frame_meta(original, output);
    Check(pa::FindFrameResult(original) == pa::FindFrameResult(output), "tee metadata did not share immutable result");
  }
  ~Metadata() {
    nvds_destroy_batch_meta(program);
    nvds_destroy_batch_meta(source);
  }
  NvDsBatchMeta* source{nullptr};
  NvDsBatchMeta* program{nullptr};
  NvDsFrameMeta* original{nullptr};
  NvDsFrameMeta* output{nullptr};
};

bool ColoredAt(const std::vector<unsigned char>& pixels, unsigned width, int x, int y) {
  const size_t offset = (size_t(y) * width + x) * 4;
  return pixels[offset] != pixels[offset + 1] || pixels[offset + 1] != pixels[offset + 2];
}

std::vector<unsigned char> Run(
    cudaStream_t stream,
    Surface& input,
    Surface& output,
    uint32_t layers,
    bool suppress,
    float confidence,
    bool invalid_output = false) {
  Metadata meta;
  Cropper cropper(stream);
  Check(cropper.SetProperty({"player-overlay-layers", std::to_string(layers)}), "layer setting rejected");
  Check(cropper.SetProperty({"player-joint-confidence", std::to_string(confidence)}), "confidence rejected");
  if (suppress)
    cropper.Suppress();
  const auto* original_result = pa::FindFrameResult(meta.original);
  if (invalid_output)
    output.value->surfaceList[0].colorFormat = NVBUF_COLOR_FORMAT_GRAY8;
  const auto status = cropper.GenerateOutput(meta.program, input.value, output.value);
  // Query before fixture readback: no implicit copy synchronization may hide
  // missing caller fences on normal, suppressed, or post-submit error returns.
  Check(cudaStreamQuery(stream) == cudaSuccess, "GenerateOutput returned with unfinished borrowed-surface work");
  output.value->surfaceList[0].colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  Check(status.ok() != invalid_output, "unexpected GenerateOutput result");
  Check(
      pa::FindFrameResult(meta.original) == original_result && original_result->baked_layers == 0,
      "Program mutated the shared semantic payload");
  Check(!preview::find_playcropper_transform_meta(meta.original), "Program transform leaked to Stitched metadata");
  Check(
      meta.original->source_frame_width == 640 && meta.original->source_frame_height == 480,
      "Program transformed source metadata in place");
  const NvDsObjectMeta* source_person = nullptr;
  for (const auto* item = meta.original->obj_meta_list; item; item = item->next) {
    const auto* object = static_cast<const NvDsObjectMeta*>(item->data);
    if (object->class_id == 0)
      source_person = object;
  }
  Check(
      source_person && source_person->rect_params.left == 240 && source_person->rect_params.width == 80,
      "Program changed source object coordinates");
  if (invalid_output) {
    Check(output.value->numFilled == 0, "failed frame was marked complete");
    const auto pixels = output.Pixels();
    cropper.Shutdown();
    Check(!cropper.allocated(), "failed caller retained compositor after Shutdown");
    cropper.Shutdown();
    Check(cudaStreamQuery(stream) == cudaSuccess, "idempotent error Shutdown left pending work");
    return pixels;
  }
  Check(output.value->numFilled == 1, "successful output was not filled");
  Check(
      cropper.allocated() == (suppress || (layers && confidence <= 0.9F)),
      "disabled or empty overlay allocated compositor resources");
  const auto* transform = preview::find_playcropper_transform_meta(meta.output);
  if (!layers) {
    Check(!transform, "all-off/no-preview frame attached overlay transform");
  } else {
    Check(transform, "semantic drawing did not publish Program transform without diagnostic snapshot");
    // Independent fixture geometry: input pixels are half metadata coordinates,
    // source x=40..220, anchor=(90,105), crop=(0,30,180,150), angle=17*(1-130/160).
    Check(
        transform->input_width == 320 && transform->input_height == 240 && transform->metadata_width == 640 &&
            transform->metadata_height == 480 && transform->source_left == 40 && transform->source_top == 0 &&
            transform->anchor_x == 90 && transform->anchor_y == 105 && transform->crop_left == 0 &&
            transform->crop_top == 30 && transform->crop_width == 180 && transform->crop_height == 150 &&
            transform->output_width == 160 && transform->output_height == 120 &&
            std::abs(transform->angle_degrees - 3.1875F) < 1e-5 && transform->object_meta_transformed,
        "Program published a transform different from its actual crop/rotation/resize");
    const bool drawn = !suppress && confidence <= 0.9F;
    Check(transform->baked_player_layers == (drawn ? layers : 0), "baked layers claim missing or suppressed pixels");
    Check(cropper.suppressions() == (suppress ? 1 : 0), "suppression was not recorded");
    overlay::CommandList preview_commands;
    overlay::BuildPlayerOverlays(meta.output, layers, confidence, transform, 160, 120, &preview_commands);
    Check(preview_commands.empty() == (drawn || confidence > 0.9F), "Program preview duplicated baked player drawing");
  }
  const auto pixels = output.Pixels();
  cropper.Shutdown();
  Check(!cropper.allocated(), "caller retained compositor after Shutdown");
  cropper.Shutdown();
  Check(cudaStreamQuery(stream) == cudaSuccess, "idempotent Shutdown left pending work");
  return pixels;
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  cudaStream_t stream = nullptr;
  try {
    Cuda(cudaSetDevice(0));
    Cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    Surface input(320, 240), output(160, 120);
    std::vector<unsigned char> original(320 * 240 * 4, 32);
    for (size_t i = 3; i < original.size(); i += 4)
      original[i] = 255;
    input.Pixels(&original);
    const auto off = Run(stream, input, output, 0, false, 0.3F);
    const auto empty = Run(stream, input, output, pa::kDrawPose, false, 1.0F);
    Check(empty == off, "empty semantic commands changed output pixels");
    const auto drawn = Run(stream, input, output, pa::kDrawPose, false, 0.3F);
    Check(drawn != off && ColoredAt(drawn, 160, 88, 64), "actual cropper did not draw at transformed joint position");
    Check(!ColoredAt(drawn, 160, 70, 55), "joint was drawn at naive scale instead of crop/rotation position");
    const auto suppressed = Run(stream, input, output, pa::kDrawPose, true, 0.3F);
    Check(suppressed == off, "capacity suppression changed the video frame");
#if !defined(__aarch64__)
    // Backing allocation remains RGBA-sized. Only its declared output format
    // changes, forcing the real caller's failure after transform submission.
    Run(stream, input, output, pa::kDrawPose, false, 0.3F, true);
#endif
    Check(input.Pixels() == original, "Program overlay wrote the shared Stitched GPU input");
    Cuda(cudaStreamDestroy(stream));
    std::cout << "Actual cropper output-only pixels, exact transform, immutable tee metadata, lazy/empty paths, "
                 "baked suppression and completion fences passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    if (stream)
      cudaStreamDestroy(stream);
    return 1;
  }
}
