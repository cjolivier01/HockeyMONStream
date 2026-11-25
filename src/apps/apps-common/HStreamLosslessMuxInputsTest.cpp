#include "hstream/src/apps/apps-common/HStreamLosslessMux.h"

#include <gst/gst.h>
#include <gstnvdsmeta.h>
#include <unistd.h>

#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace {
struct Stats {
  std::map<unsigned, unsigned> frames;
  std::map<unsigned, unsigned> expected;
  unsigned batches = 0;
  bool valid = true;
  bool stream_started = false;
  bool segment_received = false;
};

GstPadProbeReturn inspect(GstPad*, GstPadProbeInfo* info, gpointer data) {
  auto& stats = *static_cast<Stats*>(data);
  if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    const auto type = GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info));
    if (type == GST_EVENT_STREAM_START) {
      stats.stream_started = true;
      stats.segment_received = false;
    } else if (type == GST_EVENT_SEGMENT) {
      stats.valid &= stats.stream_started;
      stats.segment_received = true;
    }
    return GST_PAD_PROBE_OK;
  }
  stats.valid &= stats.stream_started && stats.segment_received;
  auto* batch = gst_buffer_get_nvds_batch_meta(GST_PAD_PROBE_INFO_BUFFER(info));
  if (!batch || !batch->num_frames_in_batch) {
    stats.valid = false;
    return GST_PAD_PROBE_OK;
  }
  std::set<unsigned> expected_sources;
  for (const auto& [source, length] : stats.expected)
    if (stats.frames[source] < length)
      expected_sources.insert(source);
  std::set<unsigned> sources;
  unsigned count = 0;
  for (auto* item = batch->frame_meta_list; item; item = item->next) {
    auto* frame = static_cast<NvDsFrameMeta*>(item->data);
    stats.valid &= sources.insert(frame->pad_index).second;
    stats.valid &= frame->frame_num == static_cast<gint>(stats.frames[frame->pad_index]++);
    stats.valid &= frame->buf_pts == gst_util_uint64_scale(frame->frame_num, GST_SECOND, 30);
    stats.valid &= frame->source_frame_width == 64 && frame->source_frame_height == 48;
    stats.valid &= frame->batch_id < batch->num_frames_in_batch;
    ++count;
  }
  stats.valid &= count == batch->num_frames_in_batch && sources == expected_sources;
  ++stats.batches;
  return GST_PAD_PROBE_OK;
}

bool run(
    const std::vector<unsigned>& lengths,
    unsigned delay_us = 0,
    bool cancel = false,
    bool delay_eos = false,
    bool sync = false,
    unsigned id_stride = 1,
    const char* format = "RGBA",
    unsigned passes = 1) {
  std::ostringstream description;
  description << "hstreamlosslessmux name=mux batch-size=" << lengths.size()
              << " frame-num-reset-on-eos=true sync-inputs=" << (sync ? "true" : "false")
              << " ! fakesink name=sink sync=false async=false enable-last-sample=false ";
  for (unsigned i = 0; i < lengths.size(); ++i) {
    description << "videotestsrc num-buffers=" << lengths[i]
                << " ! video/x-raw,format=RGBA,width=64,height=48,framerate=30/1 "
                << "! nvvideoconvert ! video/x-raw(memory:NVMM),format=" << format << " ";
    if (i + 1 == lengths.size() && delay_us)
      description << "! identity sleep-time=" << delay_us << " ";
    if (i + 1 == lengths.size() && delay_eos)
      description << "! identity name=late_eos ";
    description << "! mux.sink_" << i * id_stride << " ";
  }
  GError* error = nullptr;
  auto* pipeline = gst_parse_launch(description.str().c_str(), &error);
  if (!pipeline || error) {
    std::cerr << (error ? error->message : "No pipeline") << '\n';
    if (error)
      g_error_free(error);
    if (pipeline)
      gst_object_unref(pipeline);
    return false;
  }
  if (delay_eos) {
    auto* tail = gst_bin_get_by_name(GST_BIN(pipeline), "late_eos");
    auto* tail_pad = gst_element_get_static_pad(tail, "src");
    gst_pad_add_probe(
        tail_pad,
        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        [](GstPad*, GstPadProbeInfo* info, gpointer) {
          if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_EOS)
            g_usleep(200000);
          return GST_PAD_PROBE_OK;
        },
        nullptr,
        nullptr);
    gst_object_unref(tail_pad);
    gst_object_unref(tail);
  }
  Stats stats;
  for (unsigned i = 0; i < lengths.size(); ++i)
    stats.expected[i * id_stride] = lengths[i];
  auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  auto* pad = gst_element_get_static_pad(sink, "sink");
  gst_pad_add_probe(
      pad,
      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
      inspect,
      &stats,
      nullptr);
  gst_object_unref(pad);
  gst_object_unref(sink);
  auto* bus = gst_element_get_bus(pipeline);
  bool ok = true;
  for (unsigned pass = 0; pass < passes && ok; ++pass) {
    const bool cancel_this_pass = cancel && pass == 0;
    stats.stream_started = stats.segment_received = false;
    stats.frames.clear();
    stats.batches = 0;
    ok &= gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    auto* message = gst_bus_timed_pop_filtered(
        bus,
        cancel_this_pass ? 100 * GST_MSECOND : 10 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (!cancel_this_pass)
      ok &= message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      std::cerr << error->message << " " << (debug ? debug : "") << '\n';
      g_error_free(error);
      g_free(debug);
      ok = false;
    }
    if (message)
      gst_message_unref(message);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    ok &= stats.valid;
    if (!cancel_this_pass) {
      for (unsigned i = 0; i < lengths.size(); ++i)
        ok &= stats.frames[i * id_stride] == lengths[i];
    }
  }
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  std::cerr << "inputs=" << lengths.size() << " lengths=";
  for (auto length : lengths)
    std::cerr << length << ',';
  std::cerr << " format=" << format << " sync=" << sync << " stride=" << id_stride << " passes=" << passes
            << " delay=" << delay_us << " cancel=" << cancel << " batches=" << stats.batches
            << (ok ? " PASS\n" : " FAIL\n");
  return ok;
}
} // namespace

int main(int argc, char** argv) {
  alarm(120); // A teardown deadlock must fail too, not hang the test runner.
  gst_init(&argc, &argv);
  if (!register_hstream_lossless_nvstreammux())
    return 1;
  bool ok = true;
  for (const auto& lengths : std::vector<std::vector<unsigned>>{
           {8}, {1}, {0}, {8, 8}, {8, 3}, {3, 8}, {8, 8, 8}, {8, 8, 8, 8}, {8, 3, 6}, {0, 8, 8}, {0, 0, 0}})
    ok = run(lengths) && ok;
  ok = run({8, 8, 8}, 0, false, false, false, 1, "RGBA", 2) && ok;
  ok = run({8}, 0, false, false, true) && ok;
  ok = run({8, 8, 8}, 0, false, false, true) && ok;
  ok = run({8, 8, 8}, 0, false, false, false, 3, "NV12") && ok;
  ok = run({8}, 0, false, false, false, 1, "P010_10LE") && ok;
  ok = run({8, 8, 8}, 0, false, false, false, 1, "P010_10LE") && ok;
  ok = run({8, 8, 1}, 0, false, true) && ok;
  ok = run({4, 4, 4}, 100000) && ok;
  ok = run({8, 8, 8}, 150000, true, false, false, 1, "RGBA", 2) && ok;
  ok = run({100, 100, 100}, 500000, true) && ok;
  return ok ? 0 : 1;
}
