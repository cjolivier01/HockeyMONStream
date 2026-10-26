#include <X11/Xlib.h>
#undef Status
#include <fcntl.h>
#include <gst/gst.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "AmdPipelineOptions.h"
#include "PlaybackProgress.h"
#include "hstream/src/gst-plugins/gst-amd-program/AmdProgramGraph.h"
#include "hstream/src/gst-plugins/gst-rink-map/RinkMapOverlay.h"
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/ConfigPath.h"
#include "hstream/src/libs/common/ProcessDiagnostics.h"
#include "hstream/src/libs/common/UserConfig.h"
#include "hstream/src/libs/gpu/vaapi/VaStereoVideo.h"
#include "hstream/src/libs/media/AudioPlaylist.h"
#include "hstream/src/libs/program/AmdProgramAdmission.h"
#include "hstream/src/libs/program/PreviewOverlayConfig.h"
#include "hstream/src/libs/program/ProgramArchiveConfig.h"
#include "hstream/src/libs/program/ProgramAudioConfig.h"
#include "hstream/src/libs/program/ProgramDetectorSettings.h"
#include "hstream/src/libs/program/ProgramFileOutput.h"
#include "hstream/src/libs/program/ProgramLaunchConfig.h"
#include "hstream/src/libs/program/ProgramOutputConfig.h"
#include "hstream/src/libs/program/ProgramRuntimeControl.h"
#include "hstream/src/libs/program/ProgramScoreboardConfig.h"
#include "hstream/src/libs/program/VaProgramSink.h"
#include "hstream/src/libs/publication/Publication.h"

namespace {
static_assert(std::atomic<int>::is_always_lock_free, "Signal cancellation requires lock-free atomics");
std::atomic<int> interrupted{0};
void Stop(int signal) {
  // SIGINT is the desktop's graceful Stop. SIGTERM (including parent death)
  // always cancels, and a later SIGINT must never downgrade that cancellation.
  if (signal == SIGTERM)
    interrupted.store(signal, std::memory_order_relaxed);
  else {
    int running = 0;
    interrupted.compare_exchange_strong(running, signal, std::memory_order_relaxed);
  }
}
bool DrainRequested() {
  return interrupted.load(std::memory_order_relaxed) == SIGINT;
}
bool AbortRequested() {
  return interrupted.load(std::memory_order_relaxed) == SIGTERM;
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
      hm::media::AudioPlaylist* audio,
      const std::vector<std::unique_ptr<hm::program::VaProgramSink>>& outputs,
      uint64_t poll)
      : worker_([&, audio, poll] {
          std::unique_lock<std::mutex> lock(mutex_);
          while (!done_) {
            if (AbortRequested()) {
              source.Cancel();
              if (audio)
                audio->Cancel();
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
  hm::publication::ValidatePipelineStart(false);
  const bool encoding = args.sinks.count("ENCODE_FILE") || args.sinks.count("ENCODE_PROGRAM_4K_FILE");
  Require(hm::publication::EnvironmentManifest().empty() || encoding, "Managed Program runs require an archive sink");
  const auto root = Take(hm::baseline_config::resolve_root());
  const auto launch = Take(
      hm::program::LoadProgramLaunchConfig(
          args.game,
          args.application.empty() ? root / "ds_hockey_app_config.yaml" : std::filesystem::path(args.application),
          args.options));
  auto initial_overlays = Take(hm::pipeline_internal::ReadPreviewOverlaySelection(launch.configuration.values));
  if (const auto* value = std::getenv("HSTREAM_UI_PREVIEW_OVERLAYS")) {
    initial_overlays = {};
    std::istringstream names(value);
    std::string name;
    while (std::getline(names, name, ',')) {
      if (name.empty())
        continue;
      Require(name == "players", "AMD Program preview currently supports only the players diagnostic overlay");
      initial_overlays.players = true;
    }
  }
  Require(
      !initial_overlays.play && !initial_overlays.rink,
      "AMD Program preview currently supports only the players diagnostic overlay");
  Require(!initial_overlays.any() || args.sinks.count("RENDER"), "Preview overlays require the RENDER sink");
  hm::pipeline_internal::PreviewOverlayRuntimeState overlay_state(initial_overlays);
  hm::program::ProgramPreviewBoxes preview_boxes;
  auto rink_settings = Take(hm::rink_map::ResolveSettings(launch.configuration.values));
  std::shared_ptr<const hm::stitching::PlaybackRinkMap> output_rink_map;
  if (rink_settings.enabled && encoding) {
    auto prepared = hm::stitching::LoadPlaybackRinkMap(launch.game);
    if (prepared.ok())
      output_rink_map = *prepared;
    else
      g_printerr("HSTREAM_OUTPUT_RINK_MAP status=unavailable message=%s\n", prepared.status().ToString().c_str());
  }
  const hm::program::ProgramCalibrationRequest calibration{args.saved_stitch_time, args.expected_invalidation};
  Status(hm::program::ValidateProgramCalibrationSelection(launch.saved_game, calibration));
  Status(hm::program::ValidateAmdProgramAdmission(launch.configuration));
  const auto audio_config = Take(hm::program::ResolveProgramAudioConfig(launch.configuration, launch.frame_offsets));
  auto output_config = Take(hm::program::ResolveProgramOutputConfig(launch.configuration));
  const auto start = args.start.value_or(output_config.start_ns);
  const auto limit = args.limit;
  std::optional<hm::BitratePerPixel> source_bitrate;
  std::filesystem::path archive_directory;
  if (encoding) {
    archive_directory = hm::publication::EnvironmentWorkingDirectory();
    if (archive_directory.empty())
      archive_directory = Take(hm::user_config::output_root(launch.configuration.values)) / args.game;
    uint64_t selected_bitrate = 0, selected_pixels = 0;
    std::set<std::filesystem::path> visited;
    for (const auto& camera : launch.chapters)
      for (const auto& chapter : camera) {
        if (!visited.insert(chapter).second)
          continue;
        const auto metadata = Take(hm::media::ReadMp4VideoTrack(chapter, launch.amd.operation_timeout_ns, {}));
        if (!metadata.bitrate)
          continue;
        const auto pixels = uint64_t(metadata.width) * metadata.height;
        if (*metadata.bitrate > selected_bitrate ||
            (*metadata.bitrate == selected_bitrate && pixels < selected_pixels)) {
          selected_bitrate = *metadata.bitrate;
          selected_pixels = pixels;
          source_bitrate = hm::make_bitrate_per_pixel(selected_bitrate, metadata.width, metadata.height);
        }
      }
    if (!source_bitrate)
      g_printerr("Program source video bitrate metadata is unavailable; retaining configured archive bitrate\n");
  }
  const auto archives = Take(hm::program::ResolveProgramArchives(
      launch.configuration, args.sinks, output_config, archive_directory, source_bitrate));
  std::map<std::string, std::unique_ptr<hm::program::ProgramFileOutput>> files;
  for (const auto& [name, archive] : archives) {
    std::filesystem::create_directories(archive.path.parent_path());
    files.emplace(name, Take(hm::program::ProgramFileOutput::Create(archive.path)));
    g_print(
        "HSTREAM_OUTPUT type=archive sink=%d kind=%s existed=0 size=-1 mtime-ms=-1 codec=hevc path=%s\n",
        archive.sink_id,
        archive.kind.c_str(),
        archive.path.c_str());
    g_print(
        "HSTREAM_AMD_ARCHIVE sink=%d width=%d height=%d bitrate-kbps=%u\n",
        archive.sink_id,
        archive.output.width,
        archive.output.height,
        archive.output.bitrate_kbps);
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
  std::unique_ptr<hm::media::AudioPlaylist> audio;
  if (audio_config) {
    audio = Take(hm::media::AudioPlaylist::Create(audio_config->reader, launch.chapters[audio_config->camera]));
    Status(audio->Seek(pair->time));
  }
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
      launch.amd.maximum_inference_io_bytes,
      launch.amd.model_cache};
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
  auto processor = Take(
      hm::program::AmdProgramGraph::Create(
          {launch.game,
           detector_config,
           launch.camera,
           launch.amd.tracker,
           blend,
           launch.minimize_blend,
           launch.amd.compact_stitch_workspace,
           launch.configuration.values["ice_boundaries"]["raise_bbox_center_by_height_ratio"].as<float>(),
           launch.configuration.values["ice_boundaries"]["lower_bbox_bottom_by_height_ratio"].as<float>(),
           calibration,
           output_rink_map
               ? std::optional<hm::rink_map::Configuration>({rink_settings, output_rink_map, launch.amd.gpu_id})
               : std::nullopt},
          launch.amd.output_pool_frames,
          launch.amd.operation_timeout_ns,
          launch.amd.cancellation_poll_ns,
          [] { return AbortRequested(); }));
  GstVideoInfo output_info{};
  Require(
      gst_video_info_set_format(&output_info, GST_VIDEO_FORMAT_P010_10LE, output_config.width, output_config.height),
      "Invalid Program output geometry");
  output_info.fps_n = left.fps_n;
  output_info.fps_d = left.fps_d;
  output_info.colorimetry = left.colorimetry;
  output_info.chroma_site = GST_VIDEO_CHROMA_SITE_JPEG;
  std::vector<std::unique_ptr<hm::program::VaProgramSink>> outputs;
  std::vector<std::pair<GstVideoInfo, hm::program::ProgramRenderSettings>> render_settings;
  std::vector<bool> encoded_outputs;
  hm::program::VaProgramSink* preview_output{};
  std::optional<hm::program::VaProgramAudioConfig> sink_audio;
  if (audio_config) {
    const auto* mute = std::getenv("HSTREAM_RENDER_AUDIO_MUTED");
    sink_audio = hm::program::VaProgramAudioConfig{
        audio->format(),
        audio_config->queue_samples,
        audio_config->bitrate,
        audio_config->alsa_device,
        mute && std::string(mute) == "1"};
  }
  for (const auto& name : args.sinks) {
    const auto kind = files.count(name) ? hm::program::VaProgramSinkKind::kHevcMp4
        : name == "RENDER"              ? hm::program::VaProgramSinkKind::kPreview
                                        : hm::program::VaProgramSinkKind::kDiscard;
    auto settings = archives.count(name) ? archives.at(name).output : output_config;
    auto info = output_info;
    Require(
        gst_video_info_set_format(&info, GST_VIDEO_FORMAT_P010_10LE, settings.width, settings.height),
        "Invalid Program archive output geometry");
    info.fps_n = left.fps_n;
    info.fps_d = left.fps_d;
    info.colorimetry = left.colorimetry;
    info.chroma_site = GST_VIDEO_CHROMA_SITE_JPEG;
    const auto scoreboard =
        Take(hm::program::ResolveProgramScoreboardConfig(launch.configuration, settings.width, settings.height));
    if (scoreboard)
      settings.render.scoreboard = Take(hm::program::PrepareProgramScoreboard(
          *scoreboard,
          processor->generation(),
          processor->width(),
          processor->height(),
          settings.width,
          settings.height));
    render_settings.emplace_back(info, settings.render);
    encoded_outputs.push_back(kind == hm::program::VaProgramSinkKind::kHevcMp4);
    auto output = hm::program::VaProgramSink::Create(
        {kind,
         launch.amd.gpu_id,
         info,
         launch.amd.output_pool_frames,
         launch.amd.operation_timeout_ns,
         launch.amd.cancellation_poll_ns,
         name == "RENDER" && (args.realtime || output_config.realtime),
         files.count(name) ? files.at(name)->fd() : -1,
         settings.bitrate_kbps,
         settings.keyframe_interval,
         launch.amd.maximum_encoded_packet_bytes,
         args.preview_windows.empty() ? args.window : args.preview_windows.at("program"),
         nullptr,
         sink_audio});
    outputs.push_back(Take(std::move(output)));
    if (name == "RENDER")
      preview_output = outputs.back().get();
  }
  std::signal(SIGTERM, Stop);
  std::signal(SIGINT, Stop);
  Cancellation cancellation(*source, audio.get(), outputs, launch.amd.cancellation_poll_ns);
  auto first = pair->time;
  auto first_audio_sample = audio ? gst_util_uint64_scale_round(first, audio->format().rate, GST_SECOND) : 0;
  uint64_t frames = 0, tracks = 0, completed_video = 0, previous_segments = 0, progress_generation = 0;
  const uint64_t horizon = std::min(source->duration() - start, limit.value_or(source->duration() - start));
  uint64_t playback_position = 0;
  bool repositioned = false;
  uint64_t preview_generation = 0;
  GstClockTime next_preview_time = 0;
  bool preview_waiting = false;
  std::string preview_channel = "none";
  const auto select_preview = [&](const std::string& channel, uint64_t generation) {
    if (args.preview_windows.empty() || !preview_output) {
      g_print(
          "HSTREAM_PREVIEW channel=%s status=unavailable generation=%" G_GUINT64_FORMAT
          " message=Desktop preview windows were not configured\n",
          channel.c_str(),
          generation);
      return absl::OkStatus();
    }
    if (generation && generation <= preview_generation) {
      g_print(
          "HSTREAM_PREVIEW channel=%s status=stale generation=%" G_GUINT64_FORMAT
          " message=Newer preview selection is active\n",
          channel.c_str(),
          generation);
      return absl::OkStatus();
    }
    preview_generation = generation;
    if (channel != "none" && !args.preview_windows.count(channel)) {
      g_print(
          "HSTREAM_PREVIEW channel=%s status=failed generation=%" G_GUINT64_FORMAT
          " message=Preview channel has no configured window\n",
          channel.c_str(),
          generation);
      return absl::OkStatus();
    }
    // Other desktop channels will get their own GPU frame routing. Until then
    // report their absence explicitly, without showing a Program crop as a
    // source camera or stitched panorama.
    const bool available = channel == "program";
    const auto status = preview_output->SetPreviewVisible(available, next_preview_time);
    if (!status.ok())
      return status;
    if (!available && channel != "none" && preview_channel == "program") {
      // A failed selection alone leaves the UI's previous channel active.
      // Announce the branch we actually disabled so returning to Program
      // requests a fresh activation instead of trusting its previous frame.
      g_print(
          "HSTREAM_PREVIEW channel=program status=deactivated generation=%" G_GUINT64_FORMAT
          " message=Program preview disabled for this channel\n",
          generation);
    }
    preview_channel = available ? "program" : "none";
    preview_waiting = available;
    g_print(
        "HSTREAM_PREVIEW channel=%s status=%s generation=%" G_GUINT64_FORMAT " message=%s\n",
        channel.c_str(),
        available               ? "activated"
            : channel == "none" ? "deactivated"
                                : "unavailable",
        generation,
        available               ? "Waiting for the first Program GPU frame"
            : channel == "none" ? "Preview disabled"
                                : "This AMD preview channel is not yet available");
    return absl::OkStatus();
  };
  const auto publish_preview_ready = [&] {
    if (preview_waiting && Take(preview_output->PreviewReady())) {
      preview_waiting = false;
      g_print(
          "HSTREAM_PREVIEW channel=program status=ready generation=%" G_GUINT64_FORMAT
          " message=First GPU frame presented resolution=%dx%d\n",
          preview_generation,
          output_info.width,
          output_info.height);
    }
  };
  hm::PlaybackRateEstimator rate;
  auto rate_start = std::chrono::steady_clock::now();
  uint64_t rate_frames = 0;
  const auto reset_rates = [&] {
    rate.reset();
    rate_start = std::chrono::steady_clock::now();
    rate_frames = 0;
  };
  auto control = Take(hm::program::ProgramRuntimeControl::Create(
      STDIN_FILENO,
      launch.amd.maximum_runtime_command_bytes,
      launch.amd.cancellation_poll_ns,
      {
          [&](bool paused) {
            for (const auto& output : outputs) {
              const auto status = output->SetPaused(paused);
              if (!status.ok())
                return status;
            }
            if (!paused)
              reset_rates();
            return absl::OkStatus();
          },
          [&](bool muted) -> absl::StatusOr<size_t> {
            size_t branches = 0;
            size_t index = 0;
            for (const auto& name : args.sinks) {
              if (name == "RENDER" && sink_audio) {
                const auto status = outputs[index]->SetAudioMuted(muted);
                if (!status.ok())
                  return status;
                ++branches;
              }
              ++index;
            }
            return branches;
          },
          [](const std::string& message) { g_print("%s", message.c_str()); },
          [] { return interrupted.load(std::memory_order_relaxed) != 0; },
          [&](uint64_t generation) {
            if (generation < progress_generation) {
              g_print(
                  "HSTREAM_PROGRESS status=stale-reset generation=%" G_GUINT64_FORMAT
                  " active_generation=%" G_GUINT64_FORMAT " stage=0 instance=aggregate instances=1\n",
                  generation,
                  progress_generation);
            } else {
              if (generation > progress_generation) {
                progress_generation = generation;
                reset_rates();
              }
              g_print(
                  "HSTREAM_PROGRESS status=reset generation=%" G_GUINT64_FORMAT
                  " stage=0 instance=aggregate instances=1\n",
                  generation);
            }
            return absl::OkStatus();
          },
          [&](int64_t amount, bool relative, uint64_t generation) {
            const __int128 requested = relative ? static_cast<__int128>(playback_position) + amount : amount;
            const auto position = relative ? static_cast<uint64_t>(std::clamp<__int128>(requested, 0, horizon))
                                           : static_cast<uint64_t>(amount);
            if (encoding || position > horizon) {
              g_print(
                  "HSTREAM_SEEK status=failed generation=%" G_GUINT64_FORMAT " reason=%s\n",
                  generation,
                  encoding ? "local-render-required" : "position-out-of-range");
              return absl::OkStatus();
            }
            pair.reset();
            Status(source->Seek(start + position));
            pair = Take(source->Read());
            Status(processor->Reset());
            if (pair) {
              first = pair->time;
              if (audio) {
                Status(audio->Seek(first));
                first_audio_sample = gst_util_uint64_scale_round(first, audio->format().rate, GST_SECOND);
              }
              for (const auto& output : outputs)
                Status(output->ResetTimeline());
              next_preview_time = 0;
              preview_boxes.Reset();
              preview_waiting = !args.preview_windows.empty() && preview_channel == "program";
            }
            previous_segments = completed_video;
            repositioned = true;
            playback_position = pair ? std::min(horizon, pair->time - start) : horizon;
            reset_rates();
            g_print(
                "HSTREAM_SEEK status=ok generation=%" G_GUINT64_FORMAT " position_ns=%" G_GUINT64_FORMAT "\n",
                generation,
                playback_position);
            return absl::OkStatus();
          },
          select_preview,
          [&](const hm::pipeline_internal::PreviewOverlayCommand& command) {
            const auto& s = command.selection;
            const char* reason = !overlay_state.is_fresh(command) ? "stale-generation"
                : !preview_output                                 ? "preview-unavailable"
                : s.play || s.rink                                ? "unsupported-layer"
                                                                  : nullptr;
            if (!reason) {
              overlay_state.commit(command);
              if (!s.players)
                preview_boxes.Reset();
            }
            g_print(
                "HSTREAM_PREVIEW_OVERLAYS status=%s generation=%" G_GUINT64_FORMAT " players=%u play=%u rink=%u%s%s\n",
                reason ? "failed" : "applied",
                command.generation,
                unsigned(s.players),
                unsigned(s.play),
                unsigned(s.rink),
                reason ? " reason=" : "",
                reason ? reason : "");
            return absl::OkStatus();
          },
          [] { Stop(SIGINT); },
      }));
  g_print("HSTREAM_AMD_PREPARATION state=ready\n");
  if (!args.preview_windows.empty()) {
    // Announce protocol availability before activation/first-frame responses.
    g_print("HSTREAM_PREVIEW_RUNTIME status=ready channel=%s generation=1\n", args.preview_active.c_str());
    Status(select_preview(args.preview_active, 1));
  }
  const auto service_control = [&] {
    const auto status = control->Service();
    if (!(absl::IsCancelled(status) && DrainRequested()))
      Status(status);
    return !DrainRequested();
  };
  // A quit command queued during model preparation must not consume a first frame.
  if (!service_control())
    throw Failure(absl::CancelledError("Program stopped before its first completed frame"));
  // A queued startup seek may land exactly at EOS and leave no input pair.
  while (pair) {
    const auto completed =
        Take(processor->Process(pair->sequence, pair->cameras[0].sample.get(), pair->cameras[1].sample.get(),
                                pair->time, pair->duration));
    if (overlay_state.selection().players || output_rink_map)
      preview_boxes.Update(pair->time, completed.observation.tracks);
    for (size_t index = 0; index < outputs.size(); ++index) {
      const auto& output = outputs[index];
      std::unique_ptr<GstBuffer, decltype(&gst_buffer_unref)> buffer(Take(output->Acquire()), gst_buffer_unref);
      auto settings = render_settings[index].second;
      if (output_rink_map && encoded_outputs[index])
        settings.rink_map = std::make_shared<const hm::draw_display::RinkMapFrame>(preview_boxes.RinkFrame(
            processor->generation(), processor->width(), processor->height(), pair->time, pair->sequence));
      buffer.reset(Take(processor->Render(
          buffer.release(),
          render_settings[index].first,
          settings,
          output.get() == preview_output && overlay_state.selection().players ? &preview_boxes : nullptr)));
      Status(output->Push(buffer.release(), pair->time - first, pair->duration));
    }
    if (audio) {
      for (;;) {
        auto pcm = Take(audio->ReadUntil(pair->time + pair->duration));
        if (!pcm)
          break;
        pcm->sample -= first_audio_sample;
        for (const auto& output : outputs)
          Status(output->PushAudio(*pcm));
      }
    }
    next_preview_time = pair->time - first + 1;
    publish_preview_ready();
    ++frames;
    ++rate_frames;
    const auto segment_video = pair->time - first + pair->duration;
    Require(segment_video <= uint64_t(INT64_MAX) - previous_segments, "Completed playback exceeded its clock");
    completed_video = previous_segments + segment_video;
    tracks += completed.observation.tracks.size();
    g_print(
        "HSTREAM_AMD_FRAME sequence=%" G_GUINT64_FORMAT " time-ns=%" G_GUINT64_FORMAT " detections=%zu tracks=%zu\n",
        pair->sequence,
        pair->time,
        completed.detection_count,
        completed.observation.tracks.size());
    const auto now = std::chrono::steady_clock::now();
    const auto position = std::min(horizon, pair->time + pair->duration - start);
    playback_position = position;
    const auto remaining = horizon - position;
    const auto speed = rate.sample(position, remaining, now, std::chrono::nanoseconds(pair->duration));
    const double seconds = std::chrono::duration<double>(now - rate_start).count();
    std::ostringstream progress;
    progress << "HSTREAM_PROGRESS processed_ns=" << position << " total_ns=" << horizon << " remaining_ns=" << remaining
             << " eta_ns=";
    if (speed.eta_ns == hm::kUnknownPlaybackTime)
      progress << "unknown";
    else
      progress << speed.eta_ns;
    progress << std::fixed << std::setprecision(6) << " speed_x=" << speed.speed_x
             << " fps=" << speed.speed_x * output_info.fps_n / output_info.fps_d
             << " fps_avg=" << (seconds > 0 ? rate_frames / seconds : 0)
             << " fraction=" << static_cast<double>(position) / horizon
             << " stage=0 instance=aggregate instances=1 generation=" << progress_generation;
    g_print("%s\n", progress.str().c_str());
    if (limit && completed_video >= *limit)
      break;
    // No source, audio or output operation is waiting when a pause is
    // acknowledged. SIGSTOP can therefore never consume their stall budgets.
    repositioned = false;
    if (!service_control())
      break;
    if (!repositioned)
      pair = Take(source->Read());
    if (!pair)
      break;
  }
  if (DrainRequested())
    g_print("HSTREAM_AMD_STOP status=draining frames=%" G_GUINT64_FORMAT "\n", frames);
  for (const auto& output : outputs) {
    // A desktop Stop can SIGCONT a process whose output clocks are still
    // PAUSED. Resume the clocks before waiting for the audio/video mux EOS.
    Status(output->SetPaused(false));
    Status(output->Finish());
  }
  publish_preview_ready();
  if (AbortRequested())
    throw Failure(absl::CancelledError("Program cancelled before publication"));
  for (const auto& [name, file] : files) {
    Status(file->Commit(AbortRequested));
    const auto& archive = archives.at(name);
    hm::publication::RegisterOutput(archive.path, archive.kind, true, file->fd());
  }
  Require(!AbortRequested(), "Program cancelled before managed completion");
  hm::publication::CompletePipeline(true, start / GST_MSECOND);
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
    if (args.sinks.count("RENDER")) {
      // P010 DMA-BUF import requires EGL. GStreamer's default X11 platform is
      // GLX, which cannot negotiate this GPU-resident preview path. Select the
      // required API before GStreamer creates any worker/context threads.
      const auto* platform = g_getenv("GST_GL_PLATFORM");
      Require(
          !platform || !*platform || std::string(platform) == "egl",
          "AMD Program preview requires GST_GL_PLATFORM=egl");
      g_setenv("GST_GL_PLATFORM", "egl", TRUE);
    }
    XInitThreads();
    gst_init(&argc, &argv);
    Run(args);
  } catch (const std::exception& error) {
    hm::diagnostics::Log("amd-error", error.what());
    std::cerr << "hstream-cli: " << error.what() << '\n';
    result = interrupted ? 128 + interrupted : 1;
    try {
      hm::publication::CompletePipeline(false, 0);
    } catch (const std::exception& publication_error) {
      std::cerr << "hstream-cli could not record failed publication: " << publication_error.what() << '\n';
    }
  }
  hm::diagnostics::Finish(result);
  return result;
}
