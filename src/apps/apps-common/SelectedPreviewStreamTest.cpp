#include "hstream/src/apps/apps-common/SelectedPreviewStream.h"

#include <gst/video/video.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

struct Fixture {
  std::mutex observed;
  std::condition_variable ready;
  std::vector<GstBuffer*> frames;
  std::vector<std::string> stream_ids;
  std::vector<int> widths;
  unsigned segments{0};
  unsigned eos{0};
  unsigned flushes{0};
  bool block{false};
  bool blocked{false};
  bool release{false};
  GstFlowReturn flow{GST_FLOW_OK};
  GstElement* stream;
  GstPad* output;
  GstPad* consumer;
  std::array<GstPad*, 3> routes{};
  std::array<GstPad*, 3> inputs{};
  std::array<int, 3> input_widths{2, 2, 2};
  const std::array<const char*, 3> names{"program", "source0", "source1"};

  explicit Fixture(const char* feature = GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY) {
    stream = hm::preview::selected_stream_create("preview", "fixture-session", feature, "program");
    gst_object_ref_sink(stream);
    output = gst_element_get_static_pad(stream, "src");
    static GstStaticPadTemplate consumer_template =
        GST_STATIC_PAD_TEMPLATE("consumer", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw(ANY)"));
    consumer = gst_pad_new_from_static_template(&consumer_template, "consumer");
    g_object_set_data(G_OBJECT(consumer), "fixture", this);
    gst_pad_set_event_function(
        consumer, +[](GstPad* pad, GstObject*, GstEvent* event) -> gboolean {
          auto* self = static_cast<Fixture*>(g_object_get_data(G_OBJECT(pad), "fixture"));
          std::lock_guard<std::mutex> lock(self->observed);
          switch (GST_EVENT_TYPE(event)) {
            case GST_EVENT_STREAM_START: {
              const gchar* id = nullptr;
              gst_event_parse_stream_start(event, &id);
              self->stream_ids.emplace_back(id ? id : "");
              break;
            }
            case GST_EVENT_CAPS: {
              GstCaps* caps = nullptr;
              gst_event_parse_caps(event, &caps);
              gint width = 0;
              gst_structure_get_int(gst_caps_get_structure(caps, 0), "width", &width);
              self->widths.push_back(width);
              break;
            }
            case GST_EVENT_SEGMENT:
              ++self->segments;
              break;
            case GST_EVENT_EOS:
              ++self->eos;
              break;
            case GST_EVENT_FLUSH_START:
            case GST_EVENT_FLUSH_STOP:
              ++self->flushes;
              break;
            default:
              break;
          }
          gst_event_unref(event);
          return TRUE;
        });
    gst_pad_set_chain_function(
        consumer, +[](GstPad* pad, GstObject*, GstBuffer* buffer) {
          auto* self = static_cast<Fixture*>(g_object_get_data(G_OBJECT(pad), "fixture"));
          std::unique_lock<std::mutex> lock(self->observed);
          self->frames.push_back(buffer);
          if (self->block) {
            self->blocked = true;
            self->ready.notify_all();
            self->ready.wait(lock, [&] { return self->release; });
          }
          return self->flow;
        });
    gst_pad_set_active(consumer, TRUE);
    gst_pad_link(output, consumer);
    for (unsigned i = 0; i < routes.size(); ++i)
      add_route(i);
    GstClock* clock = gst_system_clock_obtain();
    gst_element_set_clock(stream, clock);
    gst_element_set_base_time(stream, gst_clock_get_time(clock));
    gst_object_unref(clock);
    gst_element_set_state(stream, GST_STATE_PLAYING);
  }

  ~Fixture() {
    gst_element_set_state(stream, GST_STATE_NULL);
    for (unsigned i = 0; i < routes.size(); ++i)
      remove_route(i);
    gst_pad_unlink(output, consumer);
    gst_object_unref(output);
    gst_object_unref(consumer);
    gst_object_unref(stream);
    for (auto* frame : frames)
      gst_buffer_unref(frame);
  }

  void add_route(unsigned i) {
    routes[i] = hm::preview::selected_stream_add_route(stream, names[i]);
    inputs[i] = gst_pad_new(nullptr, GST_PAD_SRC);
    gst_pad_set_active(inputs[i], TRUE);
    gst_pad_link(inputs[i], routes[i]);
  }
  void remove_route(unsigned i) {
    gst_pad_unlink(inputs[i], routes[i]);
    hm::preview::selected_stream_remove_route(stream, routes[i]);
    gst_object_unref(inputs[i]);
    gst_object_unref(routes[i]);
  }
  void segment(unsigned i, GstClockTime start = 0) {
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    segment.start = start;
    segment.time = start;
    gst_pad_push_event(inputs[i], gst_event_new_segment(&segment));
  }
  void headers(unsigned i, int width, GstClockTime start = 0) {
    input_widths[i] = width;
    gst_pad_push_event(inputs[i], gst_event_new_stream_start(names[i]));
    GstCaps* caps = gst_caps_new_simple(
        "video/x-raw",
        "format",
        G_TYPE_STRING,
        "RGB",
        "width",
        G_TYPE_INT,
        width,
        "height",
        G_TYPE_INT,
        2,
        "framerate",
        GST_TYPE_FRACTION,
        30,
        1,
        nullptr);
    gst_pad_push_event(inputs[i], gst_event_new_caps(caps));
    gst_caps_unref(caps);
    segment(i, start);
  }
  GstFlowReturn push(unsigned i, GstClockTime pts, GstBuffer** original = nullptr) {
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, input_widths[i] * 2 * 3, nullptr);
    gst_buffer_add_video_meta(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_RGB, input_widths[i], 2);
    GST_MINI_OBJECT_FLAG_SET(gst_buffer_peek_memory(buffer, 0), GST_MEMORY_FLAG_NO_SHARE);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
    if (original)
      *original = gst_buffer_ref(buffer);
    return gst_pad_push(inputs[i], buffer);
  }
};

bool switching_and_memory() {
  Fixture fixture;
  fixture.headers(0, 2, 1000 * GST_SECOND);
  fixture.headers(1, 4);
  fixture.headers(2, 8);
  if (!expect(
          fixture.stream_ids.empty() && fixture.widths.empty() && fixture.segments == 0,
          "Inactive route events must not negotiate or reset the consumer"))
    return false;
  if (!expect(hm::preview::selected_stream_select(fixture.stream, fixture.routes[0], 1), "Select Program"))
    return false;
  GstBuffer* original = nullptr;
  const auto flow = fixture.push(0, 1001 * GST_SECOND, &original);
  const auto* identity = fixture.frames.empty() ? nullptr : hm::preview::frame_identity(fixture.frames.back());
  const bool retained = flow == GST_FLOW_OK && identity && std::string(identity->channel) == "program" &&
      identity->operation == 1 && identity->sequence == 1 && identity->source_pts == 1001 * GST_SECOND &&
      identity->presentation_pts < 10 * GST_SECOND && GST_BUFFER_PTS(original) == 1001 * GST_SECOND &&
      hm::preview::source_frame(fixture.frames.back()) == original &&
      gst_buffer_get_video_meta(fixture.frames.back()) &&
      gst_buffer_get_video_meta(fixture.frames.back())->width == 2 &&
      gst_buffer_peek_memory(original, 0) == gst_buffer_peek_memory(fixture.frames.back(), 0);
  gst_buffer_unref(original);
  if (!expect(retained, "Presentation timestamps must use new headers while retaining exact NO_SHARE video memory"))
    return false;
  fixture.push(1, 0);
  hm::preview::selected_stream_select(fixture.stream, nullptr, 2);
  fixture.push(0, 1002 * GST_SECOND);
  gst_pad_push_event(fixture.inputs[1], gst_event_new_flush_start());
  gst_pad_push_event(fixture.inputs[1], gst_event_new_flush_stop(TRUE));
  fixture.headers(1, 6);
  if (!expect(
          fixture.frames.size() == 1 && fixture.widths == std::vector<int>{2} && fixture.flushes == 0,
          "Render-off and inactive caps/flush must not leak frames or events to the consumer"))
    return false;
  if (!expect(
          hm::preview::selected_stream_select(fixture.stream, fixture.routes[1], 2) &&
              !hm::preview::selected_stream_select(fixture.stream, fixture.routes[0], 2) &&
              !hm::preview::selected_stream_select(fixture.stream, fixture.routes[0], 1),
          "A switch can open its prepared operation, but stale or competing selections must fail"))
    return false;
  const auto second_flow = fixture.push(1, 0);
  const auto* second = fixture.frames.size() == 2 ? hm::preview::frame_identity(fixture.frames.back()) : nullptr;
  if (!second || fixture.stream_ids.size() != 1 || fixture.segments != 1 ||
      fixture.widths != std::vector<int>({2, 6})) {
    std::cerr << "flow=" << gst_flow_get_name(second_flow) << " frames=" << fixture.frames.size()
              << " streams=" << fixture.stream_ids.size() << " segments=" << fixture.segments << " caps=";
    for (auto width : fixture.widths)
      std::cerr << width << ',';
    std::cerr << '\n';
  }
  return expect(
      second && std::string(second->channel) == "source0" && second->operation == 2 && second->sequence == 2 &&
          second->source_pts == 0 && second->presentation_pts > identity->presentation_pts &&
          fixture.stream_ids == std::vector<std::string>{"fixture-session"} && fixture.segments == 1 &&
          fixture.widths == std::vector<int>({2, 6}),
      "Route changes must retain one stream/segment and monotonic time despite backwards source PTS");
}

bool preroll_and_paused_clock() {
  Fixture fixture;
  fixture.headers(0, 2);
  fixture.headers(1, 4);
  hm::preview::selected_stream_select(fixture.stream, fixture.routes[0], 1);
  GstClock* clock = gst_element_get_clock(fixture.stream);
  // GstPipeline distributes its clock before setting the playback base time.
  // A non-live route can already be delivering preroll during that interval.
  gst_element_set_state(fixture.stream, GST_STATE_PAUSED);
  gst_element_set_base_time(fixture.stream, 0);
  const bool preroll_dropped = fixture.push(0, 3600 * GST_SECOND) == GST_FLOW_OK && fixture.frames.empty();
  gst_element_set_base_time(fixture.stream, gst_clock_get_time(clock));
  gst_element_set_state(fixture.stream, GST_STATE_PLAYING);
  const bool started = fixture.push(0, 3601 * GST_SECOND) == GST_FLOW_OK && fixture.frames.size() == 1;
  const auto* first = started ? hm::preview::frame_identity(fixture.frames.back()) : nullptr;
  const bool current_clock = first && first->sequence == 1 && first->presentation_pts < GST_SECOND;
  if (!preroll_dropped || !current_clock) {
    if (!fixture.frames.empty())
      std::cerr << "Preroll presentation PTS=" << GST_BUFFER_PTS(fixture.frames.front())
                << " clock=" << gst_clock_get_time(clock) << '\n';
    gst_object_unref(clock);
    return expect(false, "Preroll must not seed the presentation timeline from an unstarted clock");
  }

  // Paused playback keeps its clock, but wall-clock time continues advancing.
  // Switching routes while paused must not consume time or frame identities.
  gst_element_set_state(fixture.stream, GST_STATE_PAUSED);
  gst_element_set_base_time(fixture.stream, gst_clock_get_time(clock) - 90 * GST_SECOND);
  const bool switched = hm::preview::selected_stream_select(fixture.stream, fixture.routes[1], 2);
  const bool paused_dropped = fixture.push(1, 0) == GST_FLOW_OK && fixture.frames.size() == 1;
  gst_element_set_base_time(fixture.stream, gst_clock_get_time(clock) - first->presentation_pts - GST_MSECOND);
  gst_element_set_state(fixture.stream, GST_STATE_PLAYING);
  const bool resumed = fixture.push(1, GST_SECOND) == GST_FLOW_OK && fixture.frames.size() == 2;
  const auto* second = resumed ? hm::preview::frame_identity(fixture.frames.back()) : nullptr;
  gst_object_unref(clock);
  return expect(
      switched && paused_dropped && second && second->sequence == 2 && second->operation == 2 &&
          std::string(second->channel) == "source0" && second->source_pts == GST_SECOND &&
          second->presentation_pts > first->presentation_pts && second->presentation_pts < GST_SECOND &&
          fixture.stream_ids.size() == 1 && fixture.segments == 1,
      "Resume and route replacement must preserve one current playback timeline despite backwards source PTS");
}

bool inactive_eos_and_output_eof() {
  Fixture fixture;
  for (unsigned i = 0; i < 3; ++i)
    fixture.headers(i, 2);
  hm::preview::selected_stream_select(fixture.stream, fixture.routes[1], 1);
  fixture.block = true;
  auto frame = std::async(std::launch::async, [&] { return fixture.push(1, GST_SECOND); });
  {
    std::unique_lock<std::mutex> lock(fixture.observed);
    if (!fixture.ready.wait_for(lock, std::chrono::seconds(5), [&] { return fixture.blocked; })) {
      fixture.release = true;
      fixture.ready.notify_all();
      return expect(false, "The selected frame must enter the consumer");
    }
  }
  auto inactive_eos =
      std::async(std::launch::async, [&] { return gst_pad_push_event(fixture.inputs[2], gst_event_new_eos()); });
  const bool inactive_returned = inactive_eos.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  auto authoritative_eos =
      std::async(std::launch::async, [&] { return gst_pad_push_event(fixture.inputs[0], gst_event_new_eos()); });
  const bool eof_waited = authoritative_eos.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
  {
    std::lock_guard<std::mutex> lock(fixture.observed);
    fixture.release = true;
  }
  fixture.ready.notify_all();
  const bool completed = frame.get() == GST_FLOW_OK && inactive_eos.get() && authoritative_eos.get();
  return expect(
      inactive_returned && eof_waited && completed && fixture.frames.size() == 1 && fixture.eos == 1 &&
          !hm::preview::selected_stream_select(fixture.stream, fixture.routes[1], 2),
      "Inactive EOS must not stall; only main-output EOF drains admitted work and ends the consumer");
}

bool replacement_and_failure() {
  Fixture fixture;
  fixture.headers(0, 2);
  fixture.headers(1, 2);
  gst_pad_push_event(fixture.inputs[1], gst_event_new_eos());
  if (!expect(
          fixture.eos == 0 && !hm::preview::selected_stream_select(fixture.stream, fixture.routes[1], 1),
          "Ended source routes must not be advertised as active or end Program"))
    return false;
  fixture.remove_route(1);
  fixture.add_route(1);
  fixture.headers(1, 4);
  if (!expect(
          hm::preview::selected_stream_select(fixture.stream, fixture.routes[1], 2) &&
              fixture.push(1, 0) == GST_FLOW_OK && fixture.frames.size() == 1,
          "A replacement source must register and stream while the retained consumer is playing"))
    return false;
  fixture.flow = GST_FLOW_ERROR;
  if (!expect(
          fixture.push(1, GST_SECOND) == GST_FLOW_ERROR &&
              !hm::preview::selected_stream_active(fixture.stream, fixture.routes[1], 2) &&
              !hm::preview::selected_stream_select(fixture.stream, fixture.routes[0], 3),
          "Consumer failure must be visible to native isolation and prevent reactivation"))
    return false;
  Fixture gpu("memory:NVMM");
  GstCaps* cpu_caps = gst_caps_from_string("video/x-raw,format=RGB,width=2,height=2,framerate=30/1");
  const bool rejects_cpu = !gst_pad_query_accept_caps(gpu.routes[0], cpu_caps);
  gst_caps_unref(cpu_caps);
  return expect(rejects_cpu, "A GPU target must reject system-memory caps instead of adding a CPU fallback");
}

bool source_pool_lifetime() {
  Fixture fixture;
  fixture.headers(0, 2);
  hm::preview::selected_stream_select(fixture.stream, fixture.routes[0], 1);
  GstBufferPool* pool = gst_buffer_pool_new();
  GstStructure* config = gst_buffer_pool_get_config(pool);
  gst_buffer_pool_config_set_params(config, nullptr, 12, 1, 1);
  const bool configured = gst_buffer_pool_set_config(pool, config) && gst_buffer_pool_set_active(pool, TRUE);
  GstBuffer* source = nullptr;
  bool valid = configured && gst_buffer_pool_acquire_buffer(pool, &source, nullptr) == GST_FLOW_OK;
  static const GstMetaInfo* opaque = [] {
    static const gchar* tags[] = {nullptr};
    GType api = gst_meta_api_type_register("HStreamOpaqueSourceTestMetaAPI", tags);
    return gst_meta_register(
        api,
        "HStreamOpaqueSourceTestMeta",
        sizeof(GstMeta),
        +[](GstMeta*, gpointer, GstBuffer*) -> gboolean { return TRUE; },
        nullptr,
        nullptr);
  }();
  if (valid) {
    gst_buffer_add_meta(source, opaque, nullptr);
    GST_BUFFER_PTS(source) = 42 * GST_SECOND;
    valid = gst_pad_push(fixture.inputs[0], source) == GST_FLOW_OK && fixture.frames.size() == 1 &&
        hm::preview::source_frame(fixture.frames[0]) == source &&
        gst_buffer_get_meta(hm::preview::source_frame(fixture.frames[0]), opaque->api) != nullptr;
  }
  GstBufferPoolAcquireParams params{};
  params.flags = GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT;
  GstBuffer* recycled = nullptr;
  if (configured) {
    valid &= gst_buffer_pool_acquire_buffer(pool, &recycled, &params) != GST_FLOW_OK;
    if (recycled)
      gst_buffer_unref(recycled);
    recycled = nullptr;
  }
  for (GstBuffer* frame : fixture.frames)
    gst_buffer_unref(frame);
  fixture.frames.clear();
  if (configured) {
    valid &= gst_buffer_pool_acquire_buffer(pool, &recycled, &params) == GST_FLOW_OK && recycled == source;
    if (recycled)
      gst_buffer_unref(recycled);
    gst_buffer_pool_set_active(pool, FALSE);
  }
  gst_object_unref(pool);
  return expect(valid, "A retained preview header must prevent its source pool from recycling the original buffer");
}
} // namespace

bool test_admission_operation_fence() {
  Fixture fixture;
  fixture.headers(0, 2);
  if (!hm::preview::selected_stream_select(fixture.stream, fixture.routes[0], 1))
    return false;
  std::atomic<GstClockTime> desired{1};
  std::mutex mutex;
  std::condition_variable ready;
  bool inspected = false, release = false;
  hm::preview::selected_stream_set_frame_admission(fixture.stream, [&](const char* channel, GstBuffer* frame) {
    const bool accepted = std::string(channel) == "program" && GST_BUFFER_PTS(frame) == desired.load();
    if (!inspected) {
      std::unique_lock<std::mutex> lock(mutex);
      inspected = true;
      ready.notify_all();
      ready.wait(lock, [&]() { return release; });
    }
    return accepted;
  });
  auto first = std::async(std::launch::async, [&]() { return fixture.push(0, 1); });
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!ready.wait_for(lock, std::chrono::seconds(1), [&]() { return inspected; })) {
      release = true;
      ready.notify_all();
      return expect(false, "Admission callback must inspect the first frame");
    }
  }
  desired.store(2);
  auto rearm = std::async(
      std::launch::async, [&]() { return hm::preview::selected_stream_select(fixture.stream, fixture.routes[0], 2); });
  const bool serialized = rearm.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout;
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
    ready.notify_all();
  }
  const bool completed = first.get() == GST_FLOW_OK && rearm.get();
  const bool old_operation = fixture.frames.size() == 1 && hm::preview::frame_identity(fixture.frames[0]) &&
      hm::preview::frame_identity(fixture.frames[0])->operation == 1;
  const bool stale_dropped = fixture.push(0, 1) == GST_FLOW_OK && fixture.frames.size() == 1;
  const bool current_admitted = fixture.push(0, 2) == GST_FLOW_OK && fixture.frames.size() == 2 &&
      hm::preview::frame_identity(fixture.frames[1])->operation == 2 &&
      hm::preview::frame_identity(fixture.frames[1])->sequence == 2;
  return expect(
      serialized && completed && old_operation && stale_dropped && current_admitted,
      "Admission and operation assignment must serialize; stale frames must drop without consuming identity");
}

bool test_mapping_operation_fence() {
  Fixture fixture;
  fixture.headers(0, 2);
  hm::preview::selected_stream_select(fixture.stream, fixture.routes[0], 1);
  std::promise<void> mapping, resume;
  auto released = resume.get_future().share();
  hm::preview::selected_stream_set_presentation_mapper(
      fixture.stream,
      [&](const GstSegment& segment, GstClockTime pts, const std::atomic<bool>&, GstClockTime* output) {
        mapping.set_value();
        released.wait();
        *output = gst_segment_to_running_time(&segment, GST_FORMAT_TIME, pts) + GST_SECOND;
        return GST_FLOW_OK;
      },
      [] {});
  auto pending = std::async(std::launch::async, [&] { return fixture.push(0, 0); });
  const bool entered = mapping.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  auto close =
      std::async(std::launch::async, [&] { return hm::preview::selected_stream_select(fixture.stream, nullptr, 2); });
  const bool unblocked = close.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
  resume.set_value();
  const bool closed = close.get();
  const bool dropped = pending.get() == GST_FLOW_OK && fixture.frames.empty();
  hm::preview::selected_stream_set_presentation_mapper(
      fixture.stream,
      [](const GstSegment&, GstClockTime pts, const std::atomic<bool>&, GstClockTime* output) {
        *output = pts + GST_SECOND;
        return GST_FLOW_OK;
      },
      [] {});
  const bool selected = hm::preview::selected_stream_select(fixture.stream, fixture.routes[0], 3);
  const bool pushed = fixture.push(0, 1) == GST_FLOW_OK && fixture.frames.size() == 1;
  const auto* identity = pushed ? hm::preview::frame_identity(fixture.frames[0]) : nullptr;
  return expect(
      entered && unblocked && closed && dropped && selected && identity && identity->operation == 3 &&
          identity->sequence == 1 && identity->source_pts == 1 && identity->presentation_pts == GST_SECOND + 1,
      "Mapping waits must not block route retirement or assign a retired operation's frame to its successor");
}

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (!preroll_and_paused_clock() || !test_admission_operation_fence() || !test_mapping_operation_fence())
    return 1;
  return switching_and_memory() && inactive_eos_and_output_eof() && replacement_and_failure() && source_pool_lifetime()
      ? 0
      : 1;
}
