#include <X11/Xlib.h>
#undef Status
#include <fcntl.h>
#include <gst/gst.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "AmdPipelineOptions.h"
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/ConfigPath.h"
#include "hstream/src/libs/common/ProcessDiagnostics.h"
#include "hstream/src/libs/gpu/vaapi/VaStereoVideo.h"
#include "hstream/src/libs/program/AmdProgramAdmission.h"
#include "hstream/src/libs/program/AmdProgramProcessor.h"
#include "hstream/src/libs/program/ProgramDetectorSettings.h"
#include "hstream/src/libs/program/ProgramFileOutput.h"
#include "hstream/src/libs/program/ProgramLaunchConfig.h"
#include "hstream/src/libs/program/ProgramOutputConfig.h"
#include "hstream/src/libs/program/VaProgramSink.h"

namespace {
static_assert(std::atomic<int>::is_always_lock_free, "Signal cancellation requires lock-free atomics");
std::atomic<int> interrupted{0};
void Stop(int signal) {
  interrupted.store(signal, std::memory_order_relaxed);
}
struct Failure : std::runtime_error {
  absl::Status status;
  explicit Failure(absl::Status value) : std::runtime_error(value.ToString()), status(std::move(value)) {}
};
void Status(const absl::Status& status) {
  if (!status.ok())
    throw Failure(status);
}
template <class T>
T Take(absl::StatusOr<T> result) {
  Status(result.status());
  return std::move(*result);
}
void Require(bool ok, const std::string& message) {
  if (!ok)
    throw Failure(absl::InvalidArgumentError(message));
}
class Cancellation {
 public:
  Cancellation(
      hm::gpu::VaStereoVideo& source,
      const std::vector<std::unique_ptr<hm::program::VaProgramSink>>& outputs,
      uint64_t poll)
      : worker_([&, poll] {
          std::unique_lock<std::mutex> lock(mutex_);
          while (!done_) {
            if (interrupted) {
              source.Cancel();
              for (const auto& output : outputs)
                output->Cancel();
              return;
            }
            wake_.wait_for(lock, std::chrono::nanoseconds(poll), [&] { return done_; });
          }
        }) {}
  ~Cancellation() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
    }
    wake_.notify_all();
    worker_.join();
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  bool done_{false};
  std::thread worker_;
};
hm::gpu::VaVideoReaderConfig Reader(
    const hm::program::ProgramLaunchConfig& launch,
    size_t camera,
    const std::filesystem::path& render_node) {
  const auto metadata =
      Take(hm::media::ReadMp4VideoTrack(launch.chapters[camera].at(0), launch.amd.operation_timeout_ns, {}));
  return {
      metadata.codec == hm::media::VideoCodec::kHevc ? hm::gpu::VaVideoCodec::kHevc : hm::gpu::VaVideoCodec::kH264,
      GST_VIDEO_FORMAT_P010_10LE,
      render_node,
      launch.amd.reader_queue_frames,
      launch.amd.operation_timeout_ns,
      launch.amd.cancellation_poll_ns};
}
void Run(const hm::program::AmdPipelineOptions& args) {
  const auto root = Take(hm::baseline_config::resolve_root());
  const auto launch = Take(hm::program::LoadProgramLaunchConfig(
      args.game,
      args.application.empty() ? root / "ds_hockey_app_config.yaml" : std::filesystem::path(args.application),
      args.options));
  Status(hm::program::ValidateAmdProgramAdmission(launch.configuration));
  const auto output_config = Take(hm::program::ResolveProgramOutputConfig(launch.configuration));
  const auto start = args.start.value_or(output_config.start_ns);
  const auto limit = args.limit;
  std::unique_ptr<hm::program::ProgramFileOutput> file;
  if (args.sinks.count("ENCODE_FILE")) {
    const auto path = hm::get_node(launch.configuration.values, "video_out.output_video_path");
    Require(
        path && path->IsScalar() && !path->as<std::string>().empty(),
        "AMD ENCODE_FILE requires video_out.output_video_path for its new MP4");
    file = Take(hm::program::ProgramFileOutput::Create(path->as<std::string>()));
  }
  char pci[32]{};
  const auto device = hipDeviceGetPCIBusId(pci, sizeof(pci), launch.amd.gpu_id);
  Require(device == hipSuccess, std::string("Cannot select AMD GPU: ") + hipGetErrorString(device));
  const auto render_node = std::filesystem::path(std::string("/dev/dri/by-path/pci-") + pci + "-render");
  auto source = Take(hm::gpu::VaStereoVideo::Create(
      {{Reader(launch, 0, render_node), Reader(launch, 1, render_node)}, launch.chapters, launch.frame_offsets}, {}));
  Status(source->Seek(start));
  auto pair = Take(source->Read());
  Require(pair.has_value(), "The requested start has no complete stereo frame");
  const auto& left = pair->cameras[0].info;
  const auto& right = pair->cameras[1].info;
  Require(gst_video_colorimetry_is_equal(&left.colorimetry, &right.colorimetry), "Camera colorimetry differs");
  g_print("HSTREAM_AMD_PREPARATION state=begin\n");
  const auto detector = Take(hm::program::PrepareProgramDetector(launch.inference_file, launch.inference, {}));
  const hm::detection::DetectorConfig detector_config{
      detector.onnx,
      detector.input_name,
      detector.width,
      detector.height,
      detector.classes,
      launch.amd.gpu_id,
      detector.maximum_detections,
      detector.scale,
      detector.confidence_threshold,
      detector.iou_threshold,
      detector.maintain_aspect,
      detector.symmetric_padding,
      detector.fp16 ? hm::gpu::MigraphxSession::Precision::kFloat16 : hm::gpu::MigraphxSession::Precision::kFloat32,
      launch.amd.maximum_inference_io_bytes};
  auto blend = hm::pano::BlendSettings::HardSeam();
  switch (Take(hm::stitching::ParseBlendMode(launch.blend.mode))) {
    case hm::stitching::BlendMode::kLaplacian:
      blend =
          hm::pano::BlendSettings::Laplacian(launch.configuration.values["stitching"]["max_blend_levels"].as<int>());
      break;
    case hm::stitching::BlendMode::kAlpha:
      blend = hm::pano::BlendSettings::Alpha(launch.blend.feather_fraction);
      break;
    case hm::stitching::BlendMode::kHardSeam:
      break;
  }
  auto processor = Take(hm::program::AmdProgramProcessor::Create(
      {launch.game,
       detector_config,
       launch.camera,
       launch.amd.tracker,
       blend,
       launch.minimize_blend,
       launch.amd.compact_stitch_workspace,
       launch.configuration.values["ice_boundaries"]["raise_bbox_center_by_height_ratio"].as<float>(),
       launch.configuration.values["ice_boundaries"]["lower_bbox_bottom_by_height_ratio"].as<float>()}));
  GstVideoInfo output_info{};
  Require(
      gst_video_info_set_format(&output_info, GST_VIDEO_FORMAT_P010_10LE, output_config.width, output_config.height),
      "Invalid Program output geometry");
  output_info.fps_n = left.fps_n;
  output_info.fps_d = left.fps_d;
  output_info.colorimetry = left.colorimetry;
  output_info.chroma_site = GST_VIDEO_CHROMA_SITE_JPEG;
  std::vector<std::unique_ptr<hm::program::VaProgramSink>> outputs;
  for (const auto& name : args.sinks) {
    const auto kind = name == "ENCODE_FILE" ? hm::program::VaProgramSinkKind::kHevcMp4
        : name == "RENDER"                  ? hm::program::VaProgramSinkKind::kPreview
                                            : hm::program::VaProgramSinkKind::kDiscard;
    auto output = hm::program::VaProgramSink::Create(
        {kind,
         launch.amd.gpu_id,
         output_info,
         launch.amd.output_pool_frames,
         launch.amd.operation_timeout_ns,
         launch.amd.cancellation_poll_ns,
         name == "RENDER" && (args.realtime || output_config.realtime),
         name == "ENCODE_FILE" ? file->fd() : -1,
         output_config.bitrate_kbps,
         output_config.keyframe_interval,
         launch.amd.maximum_encoded_packet_bytes,
         static_cast<uintptr_t>(args.window),
         nullptr});
    outputs.push_back(Take(std::move(output)));
  }
  std::signal(SIGTERM, Stop);
  std::signal(SIGINT, Stop);
  Cancellation cancellation(*source, outputs, launch.amd.cancellation_poll_ns);
  g_print("HSTREAM_AMD_PREPARATION state=ready\n");
  const auto first = pair->time;
  uint64_t frames = 0, tracks = 0;
  for (;;) {
    const auto completed =
        Take(processor->Process(pair->sequence, pair->cameras[0].sample.get(), pair->cameras[1].sample.get()));
    for (const auto& output : outputs) {
      std::unique_ptr<GstBuffer, decltype(&gst_buffer_unref)> buffer(Take(output->Acquire()), gst_buffer_unref);
      Status(processor->Render(buffer.get(), output_info, output_config.render));
      Status(output->Push(buffer.release(), pair->time - first, pair->duration));
    }
    ++frames;
    tracks += completed.observation.tracks.size();
    g_print(
        "HSTREAM_AMD_FRAME sequence=%" G_GUINT64_FORMAT " time-ns=%" G_GUINT64_FORMAT " detections=%zu tracks=%zu\n",
        pair->sequence,
        pair->time,
        completed.detection_count,
        completed.observation.tracks.size());
    if (limit && pair->time - first + pair->duration >= *limit)
      break;
    pair = Take(source->Read());
    if (!pair)
      break;
  }
  for (const auto& output : outputs)
    Status(output->Finish());
  if (interrupted)
    throw Failure(absl::CancelledError("Program cancelled before publication"));
  if (file)
    Status(file->Commit([] { return interrupted.load(std::memory_order_relaxed) != 0; }));
  g_print(
      "HSTREAM_AMD_COMPLETE frames=%" G_GUINT64_FORMAT " track-observations=%" G_GUINT64_FORMAT "\n", frames, tracks);
}
} // namespace

int main(int argc, char** argv) {
  if (const auto* parent = std::getenv("HSTREAM_UI_PARENT_PID")) {
    char* end{};
    const auto expected = std::strtol(parent, &end, 10);
    if (!*parent || !end || *end || expected <= 0 || expected > INT32_MAX || prctl(PR_SET_PDEATHSIG, SIGTERM) ||
        getppid() != expected) {
      std::cerr << "hstream-cli refused a stale or invalid HSTREAM_UI_PARENT_PID\n";
      return 78;
    }
  }
  hm::diagnostics::Initialize("hstream-cli", argc ? argv[0] : nullptr);
  g_set_print_handler(+[](const gchar* message) {
    hm::diagnostics::Log("stdout", message ? message : "");
    if (message) {
      std::fputs(message, stdout);
      std::fflush(stdout);
    }
  });
  g_set_printerr_handler(+[](const gchar* message) {
    hm::diagnostics::Log("stderr", message ? message : "");
    if (message) {
      std::fputs(message, stderr);
      std::fflush(stderr);
    }
  });
  g_log_set_default_handler(
      +[](const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer data) {
        const char* category = (level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL))
            ? "glib-error"
            : ((level & G_LOG_LEVEL_WARNING) ? "glib-warning" : (domain ? domain : "glib"));
        hm::diagnostics::Log(category, message ? message : "");
        if (level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_FATAL))
          hm::diagnostics::Breadcrumb(category, message ? message : "");
        g_log_default_handler(domain, level, message, data);
      },
      nullptr);
  if (!hm::diagnostics::Directory().empty())
    g_printerr("HStream diagnostics: %s\n", hm::diagnostics::Directory().c_str());
  int result = 0;
  try {
    const auto args = Take(hm::program::ParseAmdPipelineOptions({argv, argv + argc}));
    XInitThreads();
    gst_init(&argc, &argv);
    Run(args);
  } catch (const std::exception& error) {
    hm::diagnostics::Log("amd-error", error.what());
    std::cerr << "hstream-cli: " << error.what() << '\n';
    result = interrupted ? 128 + interrupted : 1;
  }
  hm::diagnostics::Finish(result);
  return result;
}
