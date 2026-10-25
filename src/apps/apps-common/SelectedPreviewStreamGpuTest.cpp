#include "hstream/src/apps/apps-common/HmGpuPreview.h"
#include "hstream/src/apps/apps-common/PreviewGpuSurface.h"
#include "hstream/src/apps/apps-common/PreviewRouteOwner.h"
#include "hstream/src/apps/apps-common/PreviewTransportFrames.h"
#include "hstream/src/apps/apps-common/SelectedPreviewStream.h"
#include "hstream/src/apps/apps-common/WebPreviewTarget.h"
#include "hstream/src/apps/apps-common/WebRtcEndpoint.h"

#include <gst/rtp/gstrtpbuffer.h>
#include <gst/video/video-event.h>
#include <gstnvdsmeta.h>
#include <nvbufsurface.h>
#include <nvdsmeta.h>

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {
struct SourceProbe {
  unsigned source{0};
  std::atomic<unsigned> frames{0};
  std::atomic<unsigned> errors{0};
};

GstPadProbeReturn add_native_metadata(GstPad*, GstPadProbeInfo* info, gpointer data) {
  auto* source = static_cast<SourceProbe*>(data);
  GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buffer || !gst_buffer_is_writable(buffer)) {
    ++source->errors;
    return GST_PAD_PROBE_DROP;
  }
  // CPU-generated test patterns are uploaded once by nvvideoconvert. From here
  // through the selector and hardware encoder, raw video stays in NVMM.
  if (source->source)
    GST_BUFFER_PTS(buffer) += 3600 * GST_SECOND;
  NvDsBatchMeta* batch = nvds_create_batch_meta(1);
  if (batch)
    batch->max_frames_in_batch = 1;
  NvDsFrameMeta* frame = batch ? nvds_acquire_frame_meta_from_pool(batch) : nullptr;
  if (!batch || !frame) {
    if (batch)
      nvds_destroy_batch_meta(batch);
    ++source->errors;
    return GST_PAD_PROBE_DROP;
  }
  frame->source_id = source->source;
  frame->pad_index = source->source;
  frame->frame_num = ++source->frames;
  frame->buf_pts = GST_BUFFER_PTS(buffer);
  frame->source_frame_width = source->source ? 96 : 64;
  frame->source_frame_height = 64;
  nvds_add_frame_meta_to_batch(batch, frame);
  auto* meta =
      gst_buffer_add_nvds_meta(buffer, batch, nullptr, nvds_batch_meta_copy_func, nvds_batch_meta_release_func);
  if (!meta) {
    nvds_destroy_batch_meta(batch);
    ++source->errors;
    return GST_PAD_PROBE_DROP;
  }
  meta->meta_type = NVDS_BATCH_GST_META;
  return GST_PAD_PROBE_OK;
}

struct Observations {
  std::mutex mutex;
  unsigned raw_frames{0};
  unsigned encoded_frames{0};
  unsigned owned_frames{0};
  unsigned painted_frames{0};
  unsigned rtp_frames{0};
  unsigned keyframe_requests{0};
  unsigned errors{0};
  bool hold_pool{true};
  std::vector<GstBuffer*> retained_surfaces;
  std::array<unsigned, 7> operations{};
  std::array<unsigned, 7> rtp_operations{};
  GstClockTime raw_pts{GST_CLOCK_TIME_NONE};
  GstClockTime encoded_pts{GST_CLOCK_TIME_NONE};
  GstClockTime first_rtp_pts{GST_CLOCK_TIME_NONE};
  std::uint32_t first_rtp_timestamp{0};
};

void observe_events(hm::preview::WebPreviewTarget& target, Observations& observation) {
  for (const auto& event : target.TakeEvents()) {
    if (event.kind != hm::preview::TransportEvent::Kind::kPacketized)
      continue;
    std::lock_guard<std::mutex> lock(observation.mutex);
    ++observation.rtp_frames;
    if (!GST_CLOCK_TIME_IS_VALID(observation.first_rtp_pts)) {
      observation.first_rtp_pts = event.frame.presentation_pts;
      observation.first_rtp_timestamp = event.rtp_timestamp;
    } else {
      const auto expected = static_cast<std::uint32_t>(
          gst_util_uint64_scale(event.frame.presentation_pts - observation.first_rtp_pts, 90000, GST_SECOND));
      const auto actual = event.rtp_timestamp - observation.first_rtp_timestamp;
      if (std::abs(static_cast<std::int32_t>(actual - expected)) > 1)
        ++observation.errors;
    }
    if (event.frame.operation < observation.rtp_operations.size())
      ++observation.rtp_operations[event.frame.operation];
    else
      ++observation.errors;
  }
}

bool paint_owned(GstBuffer* input, const hm::preview::OwnedSurfaceView& output, Observations& observation) {
  const auto* identity = hm::preview::frame_identity(input);
  GstMapInfo map{};
  if (!identity || !gst_buffer_map(input, &map, GST_MAP_READ))
    return false;
  const auto* source = map.size >= sizeof(NvBufSurface) ? reinterpret_cast<const NvBufSurface*>(map.data) : nullptr;
  const unsigned expected_width = std::string(identity->channel) == "program" ? 360 : 540;
  bool valid = source && source->surfaceList && source->surfaceList[0].dataPtr != output.rgba && output.width == 640 &&
      output.height == 360 && output.video.width == expected_width && output.video.height == 360 &&
      output.video.left == (640 - expected_width) / 2 && output.video.top == 0;
  // Test-only four-byte readback verifies cleared letterbox bars. Production
  // adapter code performs GPU-only copies and never maps raw video to the CPU.
  std::array<unsigned char, 4> border{255, 255, 255, 255};
  valid &=
      cudaMemcpyAsync(border.data(), output.rgba, border.size(), cudaMemcpyDeviceToHost, output.stream) == cudaSuccess;
  valid &= cudaStreamSynchronize(output.stream) == cudaSuccess;
  valid &= border[0] == 0 && border[1] == 0 && border[2] == 0;
  // Exercise drawing on this owned output before the encoder's independent
  // conversion stream receives it. The source's device memory stays separate.
  auto* mark = static_cast<unsigned char*>(output.rgba) + output.video.left * 4;
  valid &= cudaMemsetAsync(mark, 255, 16, output.stream) == cudaSuccess;
  gst_buffer_unmap(input, &map);
  std::lock_guard<std::mutex> lock(observation.mutex);
  ++observation.painted_frames;
  if (!valid)
    ++observation.errors;
  return valid;
}

GstPadProbeReturn observe_owned(GstPad*, GstPadProbeInfo* info, gpointer data) {
  auto& observation = *static_cast<Observations*>(data);
  auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  const auto* identity = hm::preview::frame_identity(buffer);
  std::lock_guard<std::mutex> lock(observation.mutex);
  ++observation.owned_frames;
  if (!identity || hm::preview::source_frame(buffer) || gst_buffer_get_nvds_batch_meta(buffer))
    ++observation.errors;
  if (observation.hold_pool)
    observation.retained_surfaces.push_back(gst_buffer_ref(buffer));
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn observe_raw(GstPad*, GstPadProbeInfo* info, gpointer data) {
  auto* observation = static_cast<Observations*>(data);
  GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  const auto* identity = hm::preview::frame_identity(buffer);
  GstBuffer* source = hm::preview::source_frame(buffer);
  NvDsBatchMeta* batch = gst_buffer_get_nvds_batch_meta(buffer);
  NvDsBatchMeta* original_batch = source ? gst_buffer_get_nvds_batch_meta(source) : nullptr;
  const auto* frame =
      batch && batch->frame_meta_list ? static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data) : nullptr;
  std::lock_guard<std::mutex> lock(observation->mutex);
  ++observation->raw_frames;
  bool valid = identity && source && batch && original_batch && frame && batch != original_batch &&
      gst_buffer_n_memory(buffer) == gst_buffer_n_memory(source) && gst_buffer_n_memory(source) > 0 &&
      gst_buffer_peek_memory(buffer, 0) == gst_buffer_peek_memory(source, 0) &&
      GST_BUFFER_PTS(source) == identity->source_pts && frame->buf_pts == identity->source_pts &&
      identity->presentation_pts == GST_BUFFER_PTS(buffer) && GST_BUFFER_PTS(buffer) < 15 * GST_SECOND &&
      (!GST_CLOCK_TIME_IS_VALID(observation->raw_pts) || GST_BUFFER_PTS(buffer) > observation->raw_pts);
  if (identity && identity->operation < observation->operations.size()) {
    ++observation->operations[identity->operation];
    valid &= (std::string(identity->channel) == "source0") == (frame && frame->source_id == 1);
  } else {
    valid = false;
  }
  if (!valid)
    ++observation->errors;
  observation->raw_pts = GST_BUFFER_PTS(buffer);
  return GST_PAD_PROBE_OK;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (!hm::gpu_preview::register_elements())
    return 1;
  GError* error = nullptr;
  GstElement* pipeline = gst_pipeline_new("selected-preview-gpu-fixture");
  Observations observation;
  hm::preview::WebPreviewConfig config;
  config.name = "web-preview";
  config.stream_id = "gpu-fixture";
  config.eof_channel = "program";
  config.surface = {640, 360, 30, 1, 8, 0};
  config.conversion_pool_frames = 8;
  config.bitrate = 1000000;
  config.idr_interval = 10;
  config.h264_profile = 0;
  config.encoder_tuning = 3;
  config.rtp_payload = 96;
  config.pending_frames = 64;
  config.pending_events = 4;
  config.stun_server = "";
  std::string target_error;
  auto invalid = config;
  invalid.encoder_tuning.reset();
  if (hm::preview::WebPreviewTarget::Validate(invalid, &target_error))
    return 1;
  auto target = hm::preview::WebPreviewTarget::Create(
      config,
      [&](GstBuffer* input, const hm::preview::OwnedSurfaceView& output) {
        return paint_owned(input, output, observation);
      },
      &target_error);
  if (!pipeline || !target) {
    std::cerr << "GPU target construction failed: " << target_error << '\n';
    return 1;
  }
  if (!gst_bin_add(GST_BIN(pipeline), target->bin()) || !target->SetPeer(1))
    return 1;
  GstElement* selected = gst_bin_get_by_name(GST_BIN(target->bin()), "selected");
  GstElement* surface = gst_bin_get_by_name(GST_BIN(target->bin()), "surface");
  GstPad* selected_output = gst_element_get_static_pad(selected, "src");
  gst_pad_add_probe(selected_output, GST_PAD_PROBE_TYPE_BUFFER, observe_raw, &observation, nullptr);
  gst_object_unref(selected_output);
  GstPad* owned_output = gst_element_get_static_pad(surface, "src");
  gst_pad_add_probe(owned_output, GST_PAD_PROBE_TYPE_BUFFER, observe_owned, &observation, nullptr);
  gst_object_unref(owned_output);
  gst_object_unref(selected);
  gst_object_unref(surface);

  hm::preview::RouteOwner owner;
  owner.initialize_selection("none", 0);
  std::array<SourceProbe, 2> sources;
  std::array<GstPad*, 2> route_pads{};
  std::array<std::uint64_t, 2> generations{};
  for (unsigned index = 0; index < sources.size(); ++index) {
    sources[index].source = index;
    const std::string channel = index ? "source0" : "program";
    const std::string description = "videotestsrc is-live=true num-buffers=" + std::string(index ? "90" : "120") +
        " pattern=" + (index ? "ball" : "smpte") + " ! video/x-raw,format=RGBA,width=" + (index ? "96" : "64") +
        ",height=64,framerate=30/1 ! "
        "nvvideoconvert gpu-id=0 nvbuf-memory-type=2 output-buffers=8 name=upload ! "
        "video/x-raw(memory:NVMM),format=" +
        (index ? "P010_10LE" : "RGBA") +
        " ! hmpreviewisolation name=ingress ! "
        "queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! hmpreviewisolation name=drain";
    GstElement* source = gst_parse_bin_from_description(description.c_str(), TRUE, &error);
    if (!source || error) {
      std::cerr << "GPU source fixture failed: " << (error ? error->message : "missing element") << '\n';
      return 1;
    }
    gst_bin_add(GST_BIN(pipeline), source);
    route_pads[index] = target->AddRoute(channel);
    GstPad* source_output = gst_element_get_static_pad(source, "src");
    if (gst_pad_link(source_output, route_pads[index]) != GST_PAD_LINK_OK)
      return 1;
    gst_object_unref(source_output);
    GstElement* upload = gst_bin_get_by_name(GST_BIN(source), "upload");
    GstPad* upload_output = gst_element_get_static_pad(upload, "src");
    gst_pad_add_probe(upload_output, GST_PAD_PROBE_TYPE_BUFFER, add_native_metadata, &sources[index], nullptr);
    gst_object_unref(upload_output);
    gst_object_unref(upload);
    GstElement* ingress = gst_bin_get_by_name(GST_BIN(source), "ingress");
    GstElement* drain = gst_bin_get_by_name(GST_BIN(source), "drain");
    hm::gpu_preview::set_isolation_failure_peer(drain, ingress);
    owner.register_route(
        channel,
        {
            [ingress](bool active, std::uint64_t generation) {
              hm::gpu_preview::set_isolation_active(ingress, active, generation);
            },
            [drain](bool active, std::uint64_t generation) {
              hm::gpu_preview::set_isolation_active(drain, active, generation);
            },
            [&, index, ingress, drain] {
              return hm::gpu_preview::isolation_active(ingress) && hm::gpu_preview::isolation_active(drain) &&
                  target->Active(route_pads[index], generations[index]);
            },
            [&, index](std::uint64_t generation) {
              generations[index] = generation;
              target->Select(route_pads[index], generation);
            },
            [&, index](std::uint64_t generation) {
              generations[index] = generation;
              return target->Select(route_pads[index], generation);
            },
            [&](std::uint64_t generation) { return target->Select(nullptr, generation); },
        });
    gst_object_unref(ingress);
    gst_object_unref(drain);
  }
  bool success = owner.select_current("program", 1, {}).status == hm::preview::RouteOwner::Status::kActivated;
  success &= gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  GstBus* bus = gst_element_get_bus(pipeline);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  const std::array<unsigned, 5> thresholds{24, 36, 48, 60, 80};
  const std::array<const char*, 5> selections{"source0", "program", "none", "source0", "program"};
  unsigned next = 0;
  bool eof = false;
  while (success && !eof && std::chrono::steady_clock::now() < deadline) {
    if (sources[0].frames >= 23)
      observe_events(*target, observation);
    if (next < thresholds.size() && sources[0].frames >= thresholds[next]) {
      if (next == 0) {
        std::lock_guard<std::mutex> lock(observation.mutex);
        success &= observation.retained_surfaces.size() == 8 && observation.raw_frames > 8;
        observation.hold_pool = false;
        for (auto* buffer : observation.retained_surfaces)
          gst_buffer_unref(buffer);
        observation.retained_surfaces.clear();
      }
      if (next == 3) {
        // Recreate an inactive source binding while the rest of the target
        // remains PLAYING. The existing upstream sticky caps/segment must be
        // admitted by the new ghost pad, and the endpoint identity stays fixed.
        auto* output = gst_pad_get_peer(route_pads[1]);
        success &= output && gst_pad_unlink(output, route_pads[1]);
        target->RemoveRoute(route_pads[1]);
        gst_object_unref(route_pads[1]);
        route_pads[1] = target->AddRoute("source0");
        success &= route_pads[1] && output && gst_pad_link(output, route_pads[1]) == GST_PAD_LINK_OK;
        if (output)
          gst_object_unref(output);
        auto* endpoint = target->endpoint();
        success &= target->SetPeer(2);
        auto* peer = hstream::webrtc_endpoint_reset_peer(endpoint);
        success &= peer != nullptr;
        if (peer)
          gst_object_unref(peer);
        success &= target->SetPeer(3);
        hstream::webrtc_endpoint_detach_peer(endpoint);
        success &= target->endpoint() == endpoint;
      }
      const auto result = owner.select_current(selections[next], next + 2, {});
      success &= result.status == hm::preview::RouteOwner::Status::kActivated ||
          result.status == hm::preview::RouteOwner::Status::kDeactivated;
      ++next;
    }
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, 10 * GST_MSECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    if (!message)
      continue;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      std::cerr << "GPU fixture: " << error->message << " " << (debug ? debug : "") << '\n';
      g_clear_error(&error);
      g_free(debug);
      success = false;
    } else {
      eof = true;
    }
    gst_message_unref(message);
  }
  observe_events(*target, observation);
  const auto counters = target->counters();
  observation.encoded_frames = counters.encoded;
  observation.keyframe_requests = counters.keyframe_requests;
  owner.forget_routes();
  gst_element_set_state(pipeline, GST_STATE_NULL);
  for (auto* buffer : observation.retained_surfaces)
    gst_buffer_unref(buffer);
  observation.retained_surfaces.clear();
  gst_object_unref(bus);
  for (auto* pad : route_pads) {
    target->RemoveRoute(pad);
    gst_object_unref(pad);
  }
  target.reset();
  gst_object_unref(pipeline);
  success &= eof && next == thresholds.size() && observation.errors == 0 && sources[0].errors == 0 &&
      sources[1].errors == 0 && observation.raw_frames > 40 && observation.encoded_frames > 40 &&
      observation.raw_frames > observation.owned_frames && observation.owned_frames == observation.painted_frames &&
      observation.encoded_frames <= observation.owned_frames && counters.packetized == observation.encoded_frames &&
      counters.events_dropped > 0 && counters.keyframe_failures == 0 && observation.keyframe_requests >= 5 &&
      observation.rtp_operations[1] > 0 && observation.rtp_operations[2] > 0 && observation.rtp_operations[3] > 0 &&
      observation.rtp_operations[4] == 0 && observation.rtp_operations[5] > 0 && observation.rtp_operations[6] > 0 &&
      observation.operations[1] > 0 && observation.operations[2] > 0 && observation.operations[3] > 0 &&
      observation.operations[4] == 0 && observation.operations[5] > 0 && observation.operations[6] > 0;
  std::cout << "NVMM frames=" << observation.raw_frames << " encoded=" << observation.encoded_frames
            << " owned=" << observation.owned_frames << " painted=" << observation.painted_frames
            << " RTP=" << counters.packetized << " observed=" << observation.rtp_frames
            << " event-drops=" << counters.events_dropped << " IDR-requests=" << observation.keyframe_requests
            << " errors=" << observation.errors << " source-errors=" << sources[0].errors << ',' << sources[1].errors
            << " source-frames=" << sources[0].frames << ',' << sources[1].frames << " switches=" << next
            << " eof=" << eof << '\n';
  return success ? 0 : 1;
}
