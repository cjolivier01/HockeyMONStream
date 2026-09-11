#include "src/apps/hstream-ui/CameraExperimentSource.h"

#include "hstream/src/apps/apps-common/deepstream_sources.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QUrl>

#include <gst-nvquery.h>

#include <cmath>
#include <filesystem>
#include <mutex>
#include <sstream>

// Qt must be parsed before the GL/X11 headers used by stitching.
#include "hstream/src/libs/stitching/ConfigureStitching.h"

// apps-common uses a logging category owned by its embedding application.
GST_DEBUG_CATEGORY(NVDS_APP);

namespace {
bool fail(std::string* error, const std::string& message) {
  if (error)
    *error = message;
  return false;
}

void set_batch_query_contract(GstElement* element, guint size) {
  GstPad* pad = gst_element_get_static_pad(element, "src");
  gst_pad_add_probe(
      pad,
      GST_PAD_PROBE_TYPE_QUERY_UPSTREAM,
      +[](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
        GstQuery* query = GST_PAD_PROBE_INFO_QUERY(info);
        if (gst_nvquery_is_batch_size(query)) {
          gst_nvquery_batch_size_set(query, GPOINTER_TO_UINT(data));
          return GST_PAD_PROBE_HANDLED;
        }
        if (gst_nvquery_is_numStreams_size(query)) {
          gst_nvquery_numStreams_size_set(query, GPOINTER_TO_UINT(data));
          return GST_PAD_PROBE_HANDLED;
        }
        return GST_PAD_PROBE_OK;
      },
      GUINT_TO_POINTER(size),
      nullptr);
  gst_object_unref(pad);
}
} // namespace

absl::StatusOr<hm::playtracker_replay::StitchingMedia> PrepareExperimentSources(
    const std::string& directory,
    unsigned width,
    unsigned height) {
  namespace fs = std::filesystem;
  try {
    hm::playtracker_replay::StitchingMedia media;
    media.directory = fs::absolute(directory).string();
    QFile file(QString::fromStdString((fs::path(media.directory) / "config.yaml").string()));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
      return absl::InvalidArgumentError("Select the game directory containing the recorded camera configuration");
    media.config_contents = file.readAll().toStdString();
    const YAML::Node config = YAML::Load(media.config_contents);
    const auto game = config["game"];
    for (unsigned i = 0; i < 2; ++i) {
      const char* side = i == 0 ? "left" : "right";
      const auto files = game["videos"][side];
      const auto offset = game["stitching"]["frame_offsets"][side];
      if (!files.IsSequence() || files.size() == 0 || !offset.IsScalar())
        return absl::InvalidArgumentError(
            "The game needs its recorded left/right playlists and synchronization offsets");
      auto& camera = media.cameras[i];
      for (const auto& entry : files) {
        fs::path path(entry.as<std::string>());
        if (path.is_relative())
          path = fs::path(media.directory) / path;
        if (!fs::is_regular_file(path))
          return absl::NotFoundError("Original camera chapter is missing: " + path.string());
        camera.files.push_back(path.string());
      }
      QProcess probe;
      probe.start(
          "ffprobe",
          {"-v",
           "error",
           "-select_streams",
           "v:0",
           "-show_entries",
           "stream=width,height,avg_frame_rate",
           "-of",
           "json",
           QString::fromStdString(camera.files.front())});
      if (!probe.waitForStarted(3000) || !probe.waitForFinished(10000) || probe.exitCode() != 0) {
        probe.kill();
        probe.waitForFinished(1000);
        return absl::InvalidArgumentError("Could not inspect original camera video with ffprobe");
      }
      const auto streams = QJsonDocument::fromJson(probe.readAllStandardOutput()).object()["streams"].toArray();
      if (streams.empty())
        return absl::InvalidArgumentError("Original camera file contains no video");
      const auto video = streams.first().toObject();
      const auto rate = video["avg_frame_rate"].toString().split('/');
      const double fps = rate.size() == 2 ? rate[0].toDouble() / rate[1].toDouble() : 0;
      const double frames = offset.as<double>();
      const double ns = frames / fps * GST_SECOND;
      if (!std::isfinite(fps) || fps <= 0 || !std::isfinite(ns) || ns < 0 || ns > G_MAXINT64)
        return absl::InvalidArgumentError("Camera frame rate or synchronization offset is invalid");
      const int camera_width = video["width"].toInt();
      const int camera_height = video["height"].toInt();
      if (camera_width <= 0 || camera_height <= 0)
        return absl::InvalidArgumentError("Camera video dimensions are invalid");
      camera.width = camera_width;
      camera.height = camera_height;
      // Match Configurator::apply_frame_offsets_and_sizes: offsets in YAML
      // are decimal frames, not seconds, and conversion truncates to ns.
      camera.offset_ns = static_cast<std::uint64_t>(ns);
    }
    if (media.cameras[0].offset_ns && media.cameras[1].offset_ns)
      return absl::InvalidArgumentError("One camera must have the recorded zero synchronization offset");
    const auto pipeline = config["pipeline"];
    const auto stitcher = pipeline ? pipeline["hmstitcher"] : YAML::Node();
    const auto stitching = config["stitching"];
    if (stitcher && stitcher["post-stitch-rotate-degrees"])
      media.rotation = stitcher["post-stitch-rotate-degrees"].as<double>();
    else if (stitcher && stitcher["post_stitch_rotate_degrees"])
      media.rotation = stitcher["post_stitch_rotate_degrees"].as<double>();
    else if (stitching && stitching["post_stitch_rotate_degrees"])
      media.rotation = stitching["post_stitch_rotate_degrees"].as<double>();
    const auto properties = stitcher ? stitcher["properties"] : YAML::Node();
    if (properties && properties["high-bit-depth"])
      media.high_bit_depth = properties["high-bit-depth"].as<bool>();
    if (properties && properties["exposure"])
      media.exposure = properties["exposure"].as<double>();
    if (properties && properties["shadow-lift"])
      media.shadow_lift = properties["shadow-lift"].as<double>();
    if (!std::isfinite(media.rotation) || !std::isfinite(media.exposure) || media.exposure < 0 ||
        media.exposure > 1.3 || !std::isfinite(media.shadow_lift) || media.shadow_lift < 0 || media.shadow_lift > 100)
      return absl::InvalidArgumentError("Recorded stitching rotation or color settings are invalid");
    auto artifacts = hm::stitching::lock_validated_stitching_artifacts(media.directory);
    if (!artifacts.ok())
      return artifacts.status();
    if (!artifacts->artifact_lock)
      return absl::FailedPreconditionError(
          "The recorded stitching maps are missing; select the corresponding game directory");
    if (artifacts->canvas_size.width != width || artifacts->canvas_size.height != height)
      return absl::FailedPreconditionError(
          "These stitching maps produce " + std::to_string(artifacts->canvas_size.width) + "x" +
          std::to_string(artifacts->canvas_size.height) + "; the recording uses " + std::to_string(width) + "x" +
          std::to_string(height) + ". Select the maps used for this recording.");
    gchar* digest = g_compute_checksum_for_string(
        G_CHECKSUM_SHA256, artifacts->artifact_revision.data(), artifacts->artifact_revision.size());
    media.artifact_revision = digest;
    g_free(digest);
    // Preserve the inputs if Save trial is pointed at a calibration file
    // (including a hard-link alias outside the game directory).
    for (const auto& entry : fs::directory_iterator(media.directory)) {
      const auto extension = entry.path().extension();
      const auto name = entry.path().filename();
      if (entry.is_regular_file() &&
          (extension == ".tif" || extension == ".png" || extension == ".pto" || name == "stitching_canvas_provenance" ||
           name == "stitching_generation_id"))
        media.artifact_paths.push_back(entry.path().string());
    }
    return media;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Could not prepare original camera inputs: ") + error.what());
  }
}

struct CameraExperimentSource::Impl {
  std::unique_ptr<NvDsSrcParentBin> sources = std::make_unique<NvDsSrcParentBin>();
  NvDsSourceConfig configs[2]{};
  GstElement* root{nullptr};
};

CameraExperimentSource::CameraExperimentSource() : impl_(std::make_unique<Impl>()) {}
CameraExperimentSource::~CameraExperimentSource() {
  if (impl_->root)
    gst_object_unref(impl_->root);
  release_uri_playlist_generation_state(impl_->sources.get());
  for (auto& config : impl_->configs) {
    g_free(config.uri);
    g_free(config.uri_list);
  }
}

bool CameraExperimentSource::Build(
    GstElement* graph,
    const hm::playtracker_replay::StitchingMedia& media,
    std::uint64_t start_ns,
    std::string* error) {
  auto& s = *impl_;
  static std::once_flag category;
  std::call_once(
      category, [] { GST_DEBUG_CATEGORY_INIT(NVDS_APP, "hstream-experiment", 0, "Camera experiment sources"); });
  s.root = gst_bin_new("experiment-original-cameras");
  gst_object_ref_sink(s.root);
  gst_bin_add(GST_BIN(graph), s.root);
  for (unsigned i = 0; i < 2; ++i) {
    auto& config = s.configs[i];
    config.type = NV_DS_SOURCE_URI_MULTIPLE;
    config.enable = TRUE;
    config.camera_id = config.source_id = i;
    config.camera_width = media.cameras[i].width;
    config.camera_height = media.cameras[i].height;
    config.num_extra_surfaces = 1;
    QStringList uris;
    for (const auto& path : media.cameras[i].files)
      uris.push_back(QUrl::fromLocalFile(QString::fromStdString(path)).toString(QUrl::FullyEncoded));
    if (uris.empty())
      return fail(error, "The original camera playlist is empty");
    config.uri = g_strdup(uris.front().toUtf8().constData());
    config.uri_list = g_strdup(uris.join(';').toUtf8().constData());
  }
  const bool created = create_multi_source_bin(2, s.configs, s.sources.get());
  if (s.sources->bin)
    gst_bin_add(GST_BIN(s.root), s.sources->bin);
  if (!created)
    return fail(error, "Could not create the production camera source graph");
  // Build runs on the preview worker, before any decoder starts. Callback
  // ownership transfers back to Qt's GLib context when Poll resumes it.
  if (!defer_uri_playlist_main_context_callbacks(s.sources.get()))
    return fail(error, "Could not defer camera source callbacks");
  g_object_set(s.sources->streammux, "batch-size", 2U, "sync-inputs", FALSE, nullptr);
  // The SDK mux holds its batch mutex while pushing events. A concurrent
  // converter CAPS query acquires that mutex under a pad stream lock, which
  // can deadlock a flushing seek. This graph has a fixed two-camera batch.
  set_batch_query_contract(s.sources->streammux, 2);
  if (!configure_uri_playlist_initial_offsets(
          s.sources.get(),
          media.cameras[0].offset_ns,
          media.cameras[1].offset_ns,
          media.cameras[0].offset_ns == 0 ? 0 : 1,
          start_ns) ||
      !arm_uri_playlist_initial_seeks(s.sources.get()))
    return fail(error, "The selected passage is outside the original camera playlists");
  GstElement* converter = gst_element_factory_make("nvvideoconvert", nullptr);
  GstElement* capsfilter = gst_element_factory_make("capsfilter", nullptr);
  GstElement* stitcher = gst_element_factory_make("videoprep", "experiment-stitcher");
  for (auto* element : {converter, capsfilter, stitcher})
    if (element)
      gst_bin_add(GST_BIN(s.root), element);
  if (!converter || !capsfilter || !stitcher)
    return fail(error, "Could not create the GPU stitcher");
  g_object_set(converter, "nvbuf-memory-type", 2U, "compute-hw", 1U, "output-buffers", 2U, nullptr);
  GstCaps* caps = gst_caps_from_string(
      media.high_bit_depth ? "video/x-raw(memory:NVMM),format=RGB10A2_LE" : "video/x-raw(memory:NVMM),format=RGBA");
  g_object_set(capsfilter, "caps", caps, nullptr);
  gst_caps_unref(caps);
  std::ostringstream properties;
  properties.precision(17);
  properties << "one-pass-mode=0;configure-only=0;show=0;stitch-compute-precision=fp16;expected-artifact-revision="
             << media.artifact_revision << ";left-frame-offset-ns=" << media.cameras[0].offset_ns
             << ";right-frame-offset-ns=" << media.cameras[1].offset_ns
             << ";post-stitch-rotate-degrees=" << media.rotation << ";high-bit-depth=" << media.high_bit_depth
             << ";exposure=" << media.exposure << ";shadow-lift=" << media.shadow_lift;
  g_object_set(
      stitcher,
      "plugin-type",
      "hmstitcher",
      "config-file",
      media.directory.c_str(),
      "num-output-buffers",
      2U,
      "num-batch-buffers",
      1U,
      "high-bit-depth",
      media.high_bit_depth ? TRUE : FALSE,
      "high-bit-depth-output",
      FALSE,
      "post-stitch-rotate-degrees",
      media.rotation,
      "plugin-private-config",
      properties.str().c_str(),
      nullptr);
  if (!gst_element_link_many(s.sources->bin, converter, capsfilter, stitcher, nullptr))
    return fail(error, "Could not link the original cameras to the GPU stitcher");
  // Stitching collapses the stereo batch to one panorama. Downstream
  // converters must query that output contract, not the two-camera mux.
  set_batch_query_contract(stitcher, 1);
  GstPad* output = gst_element_get_static_pad(stitcher, "src");
  const bool ghosted = gst_element_add_pad(s.root, gst_ghost_pad_new("src", output));
  gst_object_unref(output);
  return ghosted || fail(error, "Could not expose the stitched experiment frames");
}

GstElement* CameraExperimentSource::output() const {
  return impl_->root;
}
void CameraExperimentSource::Suspend() {
  suspend_uri_playlist_main_context_callbacks(impl_->sources.get());
}
void CameraExperimentSource::Resume() {
  resume_uri_playlist_main_context_callbacks(impl_->sources.get());
}
void CameraExperimentSource::Cancel() {
  cancel_uri_playlist_frame_barrier(impl_->sources.get());
}
bool CameraExperimentSource::Position() {
  return seek_uri_playlist_initial_positions(impl_->sources.get());
}
