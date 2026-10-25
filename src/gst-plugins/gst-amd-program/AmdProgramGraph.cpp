#include "hstream/src/gst-plugins/gst-amd-program/AmdProgramGraph.h"
#include "hstream/src/gst-plugins/gst-amd-program/ProgramElement.h"
#include "hstream/src/libs/program/AmdVideoStages.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video-info-dma.h>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace hm::program {
namespace {
using Buffer = std::unique_ptr<GstBuffer, decltype(&gst_buffer_unref)>;
using Sample = std::unique_ptr<GstSample, decltype(&gst_sample_unref)>;
using Clock = std::chrono::steady_clock;
struct Failure : std::exception {
  absl::Status status;
  std::string message;
  explicit Failure(absl::Status value) : status(std::move(value)), message(status.ToString()) {}
  const char* what() const noexcept override {
    return message.c_str();
  }
};
void Ok(absl::Status status) {
  if (!status.ok())
    throw Failure{std::move(status)};
}
template <class T>
T Take(absl::StatusOr<T> result) {
  Ok(result.status());
  return std::move(*result);
}
void Require(bool value, const char* error) {
  if (!value)
    throw Failure{absl::FailedPreconditionError(error)};
}

enum class Phase { kPair, kPanorama, kDetected, kFiltered, kTracked, kCamera };
struct RenderTask {
  Buffer target{nullptr, gst_buffer_unref};
  GstVideoInfo info{};
  ProgramRenderSettings settings;
  std::optional<ProgramPreviewBoxes> preview;
};
struct Frame {
  Phase phase{Phase::kPair};
  uint64_t sequence{};
  std::shared_ptr<GstSample> left, right;
  std::shared_ptr<const AmdPanoramaFrame> panorama;
  size_t detection_count{};
  tracking::ProgramObservation observation;
  hm::BBox crop;
  std::shared_ptr<RenderTask> render;
};
struct FrameMeta {
  GstMeta meta;
  std::shared_ptr<Frame>* frame;
};
GType MetaType() {
  static gsize type = 0;
  if (g_once_init_enter(&type)) {
    static const gchar* tags[] = {nullptr};
    const auto value = gst_meta_api_type_register("HmAmdProgramFrameMetaAPI", tags);
    g_once_init_leave(&type, value);
  }
  return type;
}
const GstMetaInfo* MetaInfo();
gboolean InitMeta(GstMeta* raw, gpointer, GstBuffer*) {
  reinterpret_cast<FrameMeta*>(raw)->frame = new std::shared_ptr<Frame>;
  return TRUE;
}
void FreeMeta(GstMeta* raw, GstBuffer*) {
  delete reinterpret_cast<FrameMeta*>(raw)->frame;
}
gboolean CopyMeta(GstBuffer* target, GstMeta* raw, GstBuffer*, GQuark type, gpointer data) {
  auto* meta = reinterpret_cast<FrameMeta*>(raw);
  // Cropping envelopes carry an exclusively writable VA target and cannot be
  // duplicated. A raw-byte region copy is not a valid frame envelope either.
  if (!GST_META_TRANSFORM_IS_COPY(type) || static_cast<GstMetaTransformCopy*>(data)->region || !*meta->frame ||
      (*meta->frame)->render)
    return FALSE;
  auto* copy = reinterpret_cast<FrameMeta*>(gst_buffer_add_meta(target, MetaInfo(), nullptr));
  if (!copy)
    return FALSE;
  *copy->frame = std::make_shared<Frame>(**meta->frame);
  return TRUE;
}
const GstMetaInfo* MetaInfo() {
  static gsize info = 0;
  if (g_once_init_enter(&info)) {
    const auto* value =
        gst_meta_register(MetaType(), "HmAmdProgramFrameMeta", sizeof(FrameMeta), InitMeta, FreeMeta, CopyMeta);
    g_once_init_leave(&info, reinterpret_cast<gsize>(value));
  }
  return reinterpret_cast<const GstMetaInfo*>(info);
}
std::shared_ptr<Frame>& Payload(GstBuffer* buffer) {
  auto* meta = reinterpret_cast<FrameMeta*>(gst_buffer_get_meta(buffer, MetaType()));
  Require(meta && meta->frame && *meta->frame, "Missing Program frame ownership metadata");
  return *meta->frame;
}
Frame& Get(GstBuffer* buffer, Phase expected) {
  auto& data = Payload(buffer);
  Require(data->phase == expected, "Program component received an out-of-order frame");
  Require(data.unique(), "Program component requires exclusive metadata ownership");
  return *data;
}
Buffer Envelope(std::shared_ptr<Frame> frame, GstClockTime time, GstClockTime duration) {
  Buffer buffer(gst_buffer_new(), gst_buffer_unref);
  Require(bool(buffer), "Cannot allocate Program frame envelope");
  auto* meta = reinterpret_cast<FrameMeta*>(gst_buffer_add_meta(buffer.get(), MetaInfo(), nullptr));
  Require(meta != nullptr, "Cannot allocate Program frame metadata");
  *meta->frame = std::move(frame);
  GST_BUFFER_PTS(buffer.get()) = time;
  GST_BUFFER_DURATION(buffer.get()) = duration;
  return buffer;
}

class Stitch final : public gst::Component {
 public:
  explicit Stitch(std::unique_ptr<AmdStitcher> stage) : stage_(std::move(stage)) {}
  absl::StatusOr<GstBuffer*> Process(GstBuffer* input) override {
    auto& frame = Get(input, Phase::kPair);
    frame.panorama = Take(stage_->Process(frame.left.get(), frame.right.get()));
    frame.left.reset();
    frame.right.reset();
    frame.phase = Phase::kPanorama;
    return input;
  }
  absl::Status Reset() override {
    return absl::OkStatus();
  }

 private:
  std::unique_ptr<AmdStitcher> stage_;
};
class Infer final : public gst::Component {
 public:
  explicit Infer(std::unique_ptr<AmdDetector> stage) : stage_(std::move(stage)) {}
  absl::StatusOr<GstBuffer*> Process(GstBuffer* input) override {
    auto& frame = Get(input, Phase::kPanorama);
    frame.observation.detections = Take(stage_->Process(*frame.panorama));
    frame.detection_count = frame.observation.detections.size();
    frame.phase = Phase::kDetected;
    return input;
  }
  absl::Status Reset() override {
    return absl::OkStatus();
  }

 private:
  std::unique_ptr<AmdDetector> stage_;
};
class Rink final : public gst::Component {
 public:
  explicit Rink(std::unique_ptr<tracking::ProgramRinkFilter> stage) : stage_(std::move(stage)) {}
  absl::StatusOr<GstBuffer*> Process(GstBuffer* input) override {
    auto& frame = Get(input, Phase::kDetected);
    const auto image = frame.panorama->image();
    frame.observation.detections = Take(stage_->Process(image.width, image.height, frame.observation.detections));
    frame.phase = Phase::kFiltered;
    return input;
  }
  absl::Status Reset() override {
    return absl::OkStatus();
  }

 private:
  std::unique_ptr<tracking::ProgramRinkFilter> stage_;
};
class Track final : public gst::Component {
 public:
  explicit Track(std::unique_ptr<tracking::NativeTracker> stage) : stage_(std::move(stage)) {}
  absl::StatusOr<GstBuffer*> Process(GstBuffer* input) override {
    auto& frame = Get(input, Phase::kFiltered);
    frame.observation.tracks = Take(stage_->Update(frame.sequence, frame.observation.detections));
    frame.phase = Phase::kTracked;
    return input;
  }
  absl::Status Reset() override {
    stage_->Reset();
    return absl::OkStatus();
  }

 private:
  std::unique_ptr<tracking::NativeTracker> stage_;
};
class Camera final : public gst::Component {
 public:
  explicit Camera(std::unique_ptr<tracking::ProgramCamera> stage) : stage_(std::move(stage)) {}
  absl::StatusOr<GstBuffer*> Process(GstBuffer* input) override {
    auto& frame = Get(input, Phase::kTracked);
    frame.observation.crop = Take(stage_->Process(frame.observation.tracks));
    const auto image = frame.panorama->image();
    frame.crop = frame.observation.crop.value_or(hm::BBox(0, 0, image.width, image.height));
    frame.phase = Phase::kCamera;
    return input;
  }
  absl::Status Reset() override {
    return stage_->Reset();
  }

 private:
  std::unique_ptr<tracking::ProgramCamera> stage_;
};
class Crop final : public gst::Component {
 public:
  explicit Crop(std::unique_ptr<AmdProgramRenderer> stage) : stage_(std::move(stage)) {}
  absl::StatusOr<GstBuffer*> Process(GstBuffer* input) override {
    auto& frame = Get(input, Phase::kCamera);
    Require(frame.render && frame.render.unique() && frame.render->target, "Program crop requires an exclusive output");
    auto& render = *frame.render;
    Ok(stage_->Render(
        *frame.panorama,
        frame.crop,
        render.target.get(),
        render.info,
        render.settings,
        render.preview ? &*render.preview : nullptr));
    if (render.settings.rink_map)
      Require(
          rink_map::AttachFrame(render.target.get(), *render.settings.rink_map, render.info),
          "Cannot attach output rink-map metadata");
    info_ = render.info;
    GST_BUFFER_PTS(render.target.get()) = GST_BUFFER_PTS(input);
    GST_BUFFER_DURATION(render.target.get()) = GST_BUFFER_DURATION(input);
    return render.target.release();
  }
  absl::Status Reset() override {
    return absl::OkStatus();
  }
  GstCaps* OutputCaps() const override {
    GstVideoInfoDmaDrm dma{};
    if (!gst_video_info_dma_drm_from_video_info(&dma, &info_, 0))
      return nullptr;
    return gst_video_info_dma_drm_to_caps(&dma);
  }

 private:
  std::unique_ptr<AmdProgramRenderer> stage_;
  GstVideoInfo info_{};
};

class Graph {
 public:
  GstElement* pipeline{};
  GstElement* source{};
  GstElement* sink{};
  uint64_t timeout, poll;
  std::function<bool()> cancelled;
  bool failed{false};
  Graph(uint64_t timeout, uint64_t poll, std::function<bool()> cancelled)
      : timeout(timeout), poll(poll), cancelled(std::move(cancelled)) {}
  ~Graph() {
    Close();
    if (pipeline)
      gst_object_unref(pipeline);
  }
  void Close() {
    if (pipeline)
      gst_element_set_state(pipeline, GST_STATE_NULL);
  }
  void Create(const char* name, std::vector<GstElement*> stages) {
    std::vector<std::unique_ptr<GstElement, decltype(&gst_object_unref)>> owned;
    for (auto* stage : stages)
      owned.emplace_back(stage, gst_object_unref);
    pipeline = gst_pipeline_new(name);
    Require(pipeline != nullptr, "Cannot allocate Program graph");
    // Adopt stages before anything else can fail.
    for (auto& stage : owned) {
      Require(gst_bin_add(GST_BIN(pipeline), stage.get()), "Cannot adopt Program component");
      (void)stage.release();
    }
    source = gst_element_factory_make("appsrc", "input");
    sink = gst_element_factory_make("appsink", "completed");
    if (source)
      gst_bin_add(GST_BIN(pipeline), source);
    if (sink)
      gst_bin_add(GST_BIN(pipeline), sink);
    Require(source && sink, "Missing GStreamer appsrc/appsink");
    g_object_set(source, "format", GST_FORMAT_TIME, "max-buffers", guint64(1), "block", TRUE, nullptr);
    g_object_set(sink, "sync", FALSE, "async", FALSE, "enable-last-sample", FALSE, "max-buffers", guint(1), nullptr);
    auto* caps = gst_caps_new_empty_simple("application/x-hstream-amd-frame");
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);
    auto* previous = source;
    for (auto* stage : stages) {
      Require(gst_element_link(previous, stage), "Cannot link Program components");
      previous = stage;
    }
    Require(gst_element_link(previous, sink), "Cannot link completed Program output");
    Require(
        gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Cannot start Program graph");
    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, name);
  }
  Buffer Run(Buffer buffer) {
    Require(!failed, "Program graph retired after failure");
    try {
      if (cancelled())
        throw Failure{absl::CancelledError("Program graph cancelled")};
      Require(
          gst_app_src_push_buffer(GST_APP_SRC(source), buffer.release()) == GST_FLOW_OK,
          "Program graph rejected input");
      const auto start = Clock::now();
      const auto delay = std::chrono::nanoseconds(timeout);
      const auto deadline = delay > Clock::time_point::max() - start ? Clock::time_point::max() : start + delay;
      for (;;) {
        if (cancelled())
          throw Failure{absl::CancelledError("Program graph cancelled")};
        auto* bus = gst_element_get_bus(pipeline);
        auto* error = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        gst_object_unref(bus);
        if (error) {
          GError* detail{};
          gchar* debug{};
          if (GST_MESSAGE_TYPE(error) == GST_MESSAGE_ERROR)
            gst_message_parse_error(error, &detail, &debug);
          const std::string message = detail ? detail->message : "Unexpected Program EOS";
          if (detail)
            g_error_free(detail);
          g_free(debug);
          gst_message_unref(error);
          throw Failure{absl::InternalError(message)};
        }
        const auto now = Clock::now();
        if (now >= deadline)
          throw Failure{absl::DeadlineExceededError("Program graph output timed out")};
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
        Sample sample(
            gst_app_sink_try_pull_sample(GST_APP_SINK(sink), std::min<uint64_t>(poll, remaining)), gst_sample_unref);
        if (sample)
          return Buffer(gst_buffer_ref(gst_sample_get_buffer(sample.get())), gst_buffer_unref);
      }
    } catch (...) {
      failed = true;
      Close();
      throw;
    }
  }
  void Reset() {
    Require(!failed, "Cannot reset a failed Program graph");
    Require(gst_element_set_state(pipeline, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE, "Cannot flush Program graph");
    Require(
        gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Cannot restart Program graph");
  }
};
} // namespace
struct AmdProgramGraph::Impl {
  std::unique_ptr<Graph> processing, rendering;
  std::shared_ptr<Frame> completed;
  int width, height;
  std::string generation;
  bool failed{false};
};
AmdProgramGraph::AmdProgramGraph(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AmdProgramGraph::~AmdProgramGraph() = default;
absl::StatusOr<std::unique_ptr<AmdProgramGraph>> AmdProgramGraph::Create(
    const AmdProgramProcessorConfig& config,
    size_t maximum_frames,
    uint64_t timeout_ns,
    uint64_t poll_ns,
    std::function<bool()> cancelled) {
  if (maximum_frames < 2)
    return absl::InvalidArgumentError(
        "Program graph requires at least 2 frames: completion sinks retain the previous frame");
  if (!timeout_ns || timeout_ns > INT64_MAX || !poll_ns || poll_ns > timeout_ns || !cancelled)
    return absl::InvalidArgumentError("Program graph requires explicit finite timing and cancellation");
  try {
    auto stitcher = Take(AmdStitcher::Create(config, maximum_frames));
    auto mask = Take(tracking::ProgramRinkFilter::Create(
        stitcher->rink_mask(), config.tracker.maximum_tracks, config.raise_center_ratio, config.lower_bottom_ratio));
    auto camera = Take(tracking::ProgramCamera::Create(mask->arena(), config.camera));
    auto tracker = Take(tracking::NativeTracker::Create(config.tracker));
    auto detector = Take(AmdDetector::Create(config.detector));
    auto renderer = Take(AmdProgramRenderer::Create(config.detector.device));
    auto impl = std::make_unique<Impl>();
    impl->width = stitcher->width();
    impl->height = stitcher->height();
    impl->generation = stitcher->generation();
    impl->processing = std::make_unique<Graph>(timeout_ns, poll_ns, cancelled);
    impl->rendering = std::make_unique<Graph>(timeout_ns, poll_ns, cancelled);
    // GstElement references stay owned until their graph has adopted all of them.
    std::vector<std::unique_ptr<GstElement, decltype(&gst_object_unref)>> stages;
    const auto add = [&](const char* factory, const char* name, std::unique_ptr<gst::Component> component) {
      stages.emplace_back(Take(gst::CreateElement(factory, name, std::move(component))), gst_object_unref);
    };
    add("hmamdstitch", "stitcher", std::make_unique<Stitch>(std::move(stitcher)));
    add("hmamdinfer", "primary-inference", std::make_unique<Infer>(std::move(detector)));
    add("hmrinkfilter", "field-mask", std::make_unique<Rink>(std::move(mask)));
    add("hmobjecttrack", "object-tracker", std::make_unique<Track>(std::move(tracker)));
    add("hmcameratrack", "camera-policy", std::make_unique<Camera>(std::move(camera)));
    std::vector<GstElement*> raw;
    for (auto& stage : stages)
      raw.push_back(stage.release());
    impl->processing->Create("amd-program-processing", std::move(raw));
    auto* crop = Take(gst::CreateElement("hmamdcrop", "program-crop", std::make_unique<Crop>(std::move(renderer))));
    std::vector<GstElement*> render_stages{crop};
    if (config.rink_map)
      render_stages.push_back(Take(rink_map::Create(*config.rink_map)));
    impl->rendering->Create("amd-program-rendering", std::move(render_stages));
    return std::unique_ptr<AmdProgramGraph>(new AmdProgramGraph(std::move(impl)));
  } catch (const Failure& error) {
    return error.status;
  } catch (const std::exception& error) {
    return absl::InternalError(error.what());
  }
}
absl::StatusOr<AmdProgramFrame> AmdProgramGraph::Process(
    uint64_t sequence,
    GstSample* left,
    GstSample* right,
    GstClockTime time,
    GstClockTime duration) {
  auto& impl = *impl_;
  if (impl.failed)
    return absl::FailedPreconditionError("Program graph retired after failure");
  impl.failed = true;
  impl.completed.reset();
  try {
    Require(left && right, "Program requires both source samples");
    auto frame = std::make_shared<Frame>();
    frame->sequence = sequence;
    frame->left = {gst_sample_ref(left), gst_sample_unref};
    frame->right = {gst_sample_ref(right), gst_sample_unref};
    auto output = impl.processing->Run(Envelope(std::move(frame), time, duration));
    impl.completed = Payload(output.get());
    Require(impl.completed->phase == Phase::kCamera, "Incomplete Program processing graph");
    const auto& completed = *impl.completed;
    impl.failed = false;
    return AmdProgramFrame{completed.detection_count, completed.observation, completed.crop};
  } catch (const Failure& error) {
    return error.status;
  } catch (const std::exception& error) {
    return absl::InternalError(error.what());
  }
}
absl::StatusOr<GstBuffer*> AmdProgramGraph::Render(
    GstBuffer* output,
    const GstVideoInfo& info,
    const ProgramRenderSettings& settings,
    const ProgramPreviewBoxes* preview) {
  Buffer owned(output, gst_buffer_unref);
  auto& impl = *impl_;
  if (impl.failed || !impl.completed)
    return absl::FailedPreconditionError("Program has no completed frame to render");
  impl.failed = true;
  try {
    auto frame = std::make_shared<Frame>(*impl.completed);
    frame->render = std::make_shared<RenderTask>();
    frame->render->target = std::move(owned);
    frame->render->info = info;
    frame->render->settings = settings;
    if (preview)
      frame->render->preview = *preview;
    auto result = impl.rendering->Run(Envelope(std::move(frame), GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE));
    // appsink can still hold a render/preroll reference while delivering its
    // completed sample. Only the downstream timestamp header must be writable;
    // keep the original pool buffer alive and share its finished GPU memories.
    result.reset(Take(gst::WritableHeader(result.release())));
    impl.failed = false;
    return result.release();
  } catch (const Failure& error) {
    return error.status;
  } catch (const std::exception& error) {
    return absl::InternalError(error.what());
  }
}
absl::Status AmdProgramGraph::Reset() {
  auto& impl = *impl_;
  if (impl.failed)
    return absl::FailedPreconditionError("Cannot reset a failed Program graph");
  impl.failed = true;
  try {
    impl.completed.reset();
    impl.processing->Reset();
    impl.rendering->Reset();
    impl.failed = false;
    return absl::OkStatus();
  } catch (const Failure& error) {
    return error.status;
  } catch (const std::exception& error) {
    return absl::InternalError(error.what());
  }
}
int AmdProgramGraph::width() const {
  return impl_->width;
}
int AmdProgramGraph::height() const {
  return impl_->height;
}
const std::string& AmdProgramGraph::generation() const {
  return impl_->generation;
}
} // namespace hm::program
