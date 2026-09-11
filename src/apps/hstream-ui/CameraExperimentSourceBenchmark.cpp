#include "src/apps/hstream-ui/CameraExperimentSource.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QUrl>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>

namespace {
using Clock = std::chrono::steady_clock;
struct Measurement {
  unsigned limit{0};
  std::atomic<unsigned> frames{0};
  Clock::time_point first, last;
  std::atomic<bool> finished{false};
};
GstPadProbeReturn count(GstPad* pad, GstPadProbeInfo*, gpointer data) {
  auto& measurement = *static_cast<Measurement*>(data);
  const auto frame = measurement.frames.fetch_add(1);
  if (!frame)
    measurement.first = Clock::now();
  if (measurement.limit && frame >= measurement.limit) {
    if (frame == measurement.limit)
      gst_pad_push_event(pad, gst_event_new_eos());
    return GST_PAD_PROBE_DROP;
  }
  measurement.last = Clock::now();
  return GST_PAD_PROBE_OK;
}
bool run(GstElement* graph, CameraExperimentSource* source, Measurement& measurement, const char* name) {
  GstBus* bus = gst_element_get_bus(graph);
  if (source)
    source->Resume();
  const auto start = Clock::now();
  gst_element_set_state(graph, GST_STATE_PLAYING);
  bool ok = false;

  while (Clock::now() - start < std::chrono::minutes(3)) {
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
    if (source)
      source->Position();
    if (measurement.finished.load()) {
      ok = true;
      break;
    }
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, 10 * GST_MSECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (!message)
      continue;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
      ok = true;
    } else {
      GError* error = nullptr;
      gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      std::cerr << name << ": " << error->message << "\n" << (debug ? debug : "") << '\n';
      g_clear_error(&error);
      g_free(debug);
    }
    gst_message_unref(message);
    break;
  }
  if (source) {
    source->Suspend();
    source->Cancel();
  }
  gst_element_set_state(graph, GST_STATE_NULL);
  gst_object_unref(bus);
  if (ok) {
    const unsigned frames = std::min(measurement.limit, measurement.frames.load());
    const double seconds = std::chrono::duration<double>(measurement.last - measurement.first).count();
    if (frames < 2 || seconds <= 0)
      return false;
    std::cout << "BENCHMARK " << name << " frames=" << frames << " steady_fps=" << (frames - 1) / seconds
              << " total_seconds=" << std::chrono::duration<double>(Clock::now() - start).count() << std::endl;
  }
  return ok;
}
} // namespace

// Explicit real-media benchmark. The only CPU video transfer in this program
// is the optional, bounded FFV1 cache comparison. It is not a production sink.
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (argc != 8) {
    std::cerr << "GAME WIDTH HEIGHT START_SECONDS FRAME_COUNT OUTPUT.mkv MODE(direct|cache|decode)\n";
    return 2;
  }
  const std::string mode(argv[7]);
  unsigned width = 0, height = 0, limit = 0;
  double start_seconds = 0;
  try {
    width = std::stoul(argv[2]);
    height = std::stoul(argv[3]);
    limit = std::stoul(argv[5]);
    start_seconds = std::stod(argv[4]);
  } catch (const std::exception&) {
    std::cerr << "Invalid benchmark dimensions, start time or frame count\n";
    return 2;
  }
  if (!width || !height || limit < 2 || limit > 10000 || !std::isfinite(start_seconds) || start_seconds < 0 ||
      start_seconds > static_cast<double>(G_MAXINT64) / GST_SECOND ||
      (mode != "direct" && mode != "cache" && mode != "decode")) {
    std::cerr << "Use positive dimensions, a nonnegative start and 2–10000 frames\n";
    return 2;
  }
  g_setenv("USE_NEW_NVSTREAMMUX", "yes", TRUE);
  gst_init(nullptr, nullptr);
  const QDir executable(QCoreApplication::applicationDirPath());
  GstPlugin* plugin = gst_plugin_load_file(
      executable.filePath("../../gst-plugins/gst-videoprep/libnvdsgst_videoprep.so").toUtf8(), nullptr);
  if (!plugin)
    return 2;
  gst_object_unref(plugin);
  GstElement* graph = gst_pipeline_new("experiment-source-benchmark");
  std::unique_ptr<CameraExperimentSource> source;
  GstElement* head = nullptr;
  GstElement* sink = gst_element_factory_make(mode == "cache" ? "filesink" : "fakesink", nullptr);
  g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
  gst_bin_add(GST_BIN(graph), sink);
  Measurement measurement;
  measurement.limit = limit;
  if (mode == "decode") {
    GstElement* decode = gst_element_factory_make("uridecodebin", nullptr);
    GstElement* upload = gst_element_factory_make("nvvideoconvert", nullptr);
    GstElement* caps = gst_element_factory_make("capsfilter", nullptr);
    gst_bin_add_many(GST_BIN(graph), decode, upload, caps, nullptr);
    const auto uri = QUrl::fromLocalFile(argv[6]).toString(QUrl::FullyEncoded).toUtf8();
    g_object_set(decode, "uri", uri.constData(), nullptr);
    g_signal_connect(
        decode,
        "pad-added",
        G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer data) {
          GstPad* target = gst_element_get_static_pad(static_cast<GstElement*>(data), "sink");
          gst_pad_link(pad, target);
          gst_object_unref(target);
        }),
        upload);
    GstCaps* rgba = gst_caps_from_string("video/x-raw(memory:NVMM),format=RGBA");
    g_object_set(caps, "caps", rgba, nullptr);
    gst_caps_unref(rgba);
    g_object_set(upload, "nvbuf-memory-type", 2U, "output-buffers", 2U, nullptr);
    if (!gst_element_link_many(upload, caps, sink, nullptr))
      return 2;
    head = caps;
  } else {
    auto media = PrepareExperimentSources(argv[1], width, height);
    if (!media.ok()) {
      std::cerr << media.status() << '\n';
      return 2;
    }
    source = std::make_unique<CameraExperimentSource>();
    std::string error;
    if (!source->Build(graph, *media, start_seconds * GST_SECOND, &error)) {
      std::cerr << error << '\n';
      return 2;
    }
    head = source->output();
    if (mode == "cache") {
      GstElement* download = gst_element_factory_make("nvvideoconvert", nullptr);
      GstElement* caps = gst_element_factory_make("capsfilter", nullptr);
      GstElement* encoder = gst_element_factory_make("avenc_ffv1", nullptr);
      GstElement* mux = gst_element_factory_make("matroskamux", nullptr);
      gst_bin_add_many(GST_BIN(graph), download, caps, encoder, mux, nullptr);
      GstCaps* cpu = gst_caps_from_string("video/x-raw,format=I420");
      g_object_set(caps, "caps", cpu, nullptr);
      gst_caps_unref(cpu);
      g_object_set(encoder, "threads", 0, "slices", 16, nullptr);
      g_object_set(sink, "location", argv[6], nullptr);
      if (!gst_element_link_many(head, download, caps, encoder, mux, sink, nullptr))
        return 2;
    } else if (mode == "direct") {
      if (!gst_element_link(head, sink))
        return 2;
    } else {
      return 2;
    }
  }
  GstPad* pad = gst_element_get_static_pad(head, "src");
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, count, &measurement, nullptr);
  gst_object_unref(pad);
  GstPad* terminal = gst_element_get_static_pad(sink, "sink");
  gst_pad_add_probe(
      terminal,
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      +[](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
        if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_EOS)
          static_cast<Measurement*>(data)->finished.store(true);
        return GST_PAD_PROBE_OK;
      },
      &measurement,
      nullptr);
  gst_object_unref(terminal);
  const bool ok = run(graph, source.get(), measurement, mode.c_str());
  gst_object_unref(graph);
  return ok ? 0 : 1;
}
