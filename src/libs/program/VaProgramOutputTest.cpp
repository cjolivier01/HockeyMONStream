// Test-only bounded upload/readback around the production GPU crop and VA pool.
#include "hstream/src/libs/program/VaProgramOutput.h"
#include <gst/codecparsers/gsth264parser.h>
#include <gst/codecparsers/gsth265parser.h>
#include "hstream/src/libs/gpu/vaapi/VaHipFrame.h"
#include "hstream/src/libs/gpu/vaapi/VaOutputPool.h"
#include "hstream/src/libs/program/EncodedColorimetry.h"
#include "hstream/src/libs/program/ProgramGpu.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {
void Check(bool ok, const std::string& message) {
  if (!ok) {
    std::cerr << message << std::endl;
    std::abort();
  }
}
void Hip(hipError_t status) {
  Check(status == hipSuccess, hipGetErrorString(status));
}
// Inspect only compressed NAL units. Verify VUI independently of caps, which
// some decoders inherit even when the stream itself omits color signaling.
struct EncodedChecks {
  bool hevc;
  GstH264NalParser* avc{gst_h264_nal_parser_new()};
  GstH265Parser* h265{gst_h265_parser_new()};
  std::vector<std::vector<uint8_t>> slices;
  GstClockTime pts{}, dts{}, duration{};
  unsigned headers{0}, packets{0};
  ~EncodedChecks() {
    gst_h264_nal_parser_free(avc);
    gst_h265_parser_free(h265);
  }
  template <class Vui>
  void Color(const Vui& vui) {
    Check(
        vui.video_signal_type_present_flag && vui.colour_description_present_flag && !vui.video_full_range_flag &&
            vui.colour_primaries == 1 && vui.transfer_characteristics == 1 && vui.matrix_coefficients == 1 &&
            vui.chroma_loc_info_present_flag && vui.chroma_sample_loc_type_top_field == 1 &&
            vui.chroma_sample_loc_type_bottom_field == 1,
        "Encoded SPS lost source color or centered chroma");
    ++headers;
  }
  void Inspect(GstBuffer* buffer, bool after) {
    GstMapInfo map{};
    Check(gst_buffer_map(buffer, &map, GST_MAP_READ), "Cannot inspect compressed test bytes");
    std::vector<std::vector<uint8_t>> payloads;
    for (size_t offset = 0; offset < map.size;) {
      unsigned start, length;
      bool vcl;
      if (hevc) {
        GstH265NalUnit nal{};
        auto result = gst_h265_parser_identify_nalu(h265, map.data, offset, map.size, &nal);
        Check(result == GST_H265_PARSER_OK || result == GST_H265_PARSER_NO_NAL_END, "Invalid HEVC NAL");
        start = nal.offset;
        length = nal.size;
        vcl = nal.type <= 31;
        if (after && nal.type == GST_H265_NAL_SPS) {
          GstH265SPS sps{};
          Check(gst_h265_parser_parse_sps(h265, &nal, &sps, TRUE) == GST_H265_PARSER_OK, "Invalid HEVC SPS");
          Check(sps.vui_parameters_present_flag, "Missing HEVC VUI");
          Color(sps.vui_params);
        } else if (after && nal.type == GST_H265_NAL_VPS)
          Check(gst_h265_parser_parse_nal(h265, &nal) == GST_H265_PARSER_OK, "Invalid HEVC VPS");
      } else {
        GstH264NalUnit nal{};
        auto result = gst_h264_parser_identify_nalu(avc, map.data, offset, map.size, &nal);
        Check(result == GST_H264_PARSER_OK || result == GST_H264_PARSER_NO_NAL_END, "Invalid H264 NAL");
        start = nal.offset;
        length = nal.size;
        vcl = nal.type == 1 || nal.type == 5;
        if (after && nal.type == GST_H264_NAL_SPS) {
          GstH264SPS sps{};
          Check(gst_h264_parser_parse_sps(avc, &nal, &sps) == GST_H264_PARSER_OK, "Invalid H264 SPS");
          Check(sps.vui_parameters_present_flag, "Missing H264 VUI");
          Color(sps.vui_parameters);
          gst_h264_sps_clear(&sps);
        }
      }
      Check(length && start + length > offset && start + length <= map.size, "Invalid NAL bounds");
      if (vcl)
        payloads.emplace_back(map.data + start, map.data + start + length);
      offset = start + length;
    }
    gst_buffer_unmap(buffer, &map);
    if (after) {
      Check(payloads == slices, "Color metadata changed encoded slice payloads");
      Check(
          GST_BUFFER_PTS(buffer) == pts && GST_BUFFER_DTS(buffer) == dts && GST_BUFFER_DURATION(buffer) == duration,
          "Color metadata changed encoded timing");
      ++packets;
    } else {
      slices = std::move(payloads);
      pts = GST_BUFFER_PTS(buffer);
      dts = GST_BUFFER_DTS(buffer);
      duration = GST_BUFFER_DURATION(buffer);
    }
  }
};
struct Results {
  std::atomic<unsigned> frames{0};
};
GstFlowReturn Read(GstAppSink* sink, gpointer data) {
  auto& results = *static_cast<Results*>(data);
  auto* sample = gst_app_sink_pull_sample(sink);
  Check(sample, "missing encoded sample");
  auto* buffer = gst_sample_get_buffer(sample);
  GstVideoInfo info{};
  Check(gst_video_info_from_caps(&info, gst_sample_get_caps(sample)), "missing decoded caps");
  // Test-only 384x192 decoded result. Neither the pool nor cropper maps video.
  GstVideoFrame frame{};
  Check(gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ), "cannot inspect encoded test frame");
  const bool ten_bit = GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_P010_10LE;
  const auto sample_code = [&](int plane, int x, int y) {
    const auto* p = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, plane)) +
        y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, plane);
    return ten_bit ? double(reinterpret_cast<const uint16_t*>(p)[x] >> 6) / 4 : double(p[x]);
  };
  const auto sequence = results.frames.load();
  const bool red = sequence % 2 == 0;
  const double luma = red ? .2126 : .0722;
  const double expected_y = 16 + 219 * luma;
  const double expected_u = 128 + 224 * ((red ? 0.0 : 1.0) - luma) / (2 * (1 - .0722));
  const double expected_v = 128 + 224 * ((red ? 1.0 : 0.0) - luma) / (2 * (1 - .2126));
  Check(
      std::abs(sample_code(0, 192, 96) - expected_y) < 8 && std::abs(sample_code(1, 192, 48) - expected_u) < 8 &&
          std::abs(sample_code(1, 193, 48) - expected_v) < 8,
      "crop selection or encoded YUV pixels incorrect");
  // Use 25 fps (an integral nanosecond period) to isolate timestamp ownership
  // from parser fractional-frame rounding. VA encoders offset their segment to leave room for negative DTS. Compare
  // content time through the actual output segment, not that private PTS origin.
  const auto content_time =
      gst_segment_to_stream_time(gst_sample_get_segment(sample), GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
  Check(content_time == gst_util_uint64_scale(sequence, GST_SECOND, 25), "output content timestamp changed");
  gst_video_frame_unmap(&frame);
  gst_sample_unref(sample);
  ++results.frames;
  return GST_FLOW_OK;
}
} // namespace
int main(int argc, char** argv) {
  const bool ten_bit = argc == 2 && std::string(argv[1]) == "--p010";
  gst_init(&argc, &argv);
  Hip(hipSetDevice(0));
  GstVideoInfo info{};
  Check(
      gst_video_info_set_format(&info, ten_bit ? GST_VIDEO_FORMAT_P010_10LE : GST_VIDEO_FORMAT_NV12, 384, 192),
      "output format");
  info.fps_n = 25;
  info.fps_d = 1;
  info.chroma_site = GST_VIDEO_CHROMA_SITE_JPEG;
  Check(gst_video_colorimetry_from_string(&info.colorimetry, "bt709"), "output colorimetry");
  auto created = hm::gpu::VaOutputPool::Create(0, info, 4);
  Check(created.ok(), created.status().ToString());
  auto pool = std::move(*created);
  std::vector<GstBuffer*> held;
  for (int i = 0; i < 4; ++i) {
    auto frame = pool->Acquire();
    Check(frame.ok(), frame.status().ToString());
    held.push_back(*frame);
  }
  Check(absl::IsResourceExhausted(pool->Acquire().status()), "pool must be bounded and nonblocking");
  for (auto* frame : held)
    gst_buffer_unref(frame);
  info.colorimetry.transfer = GST_VIDEO_TRANSFER_SMPTE2084;
  Check(!hm::gpu::VaOutputPool::Create(0, info, 4).ok(), "HDR output requires tone mapping");

  info.colorimetry.transfer = GST_VIDEO_TRANSFER_BT709;
  GError* error{};
  const std::string codec = ten_bit ? "h265" : "h264";
  const std::string graph = "appsrc name=input format=time ! va" + codec +
      "enc name=encoder b-frames=0 key-int-max=1 ! " + codec + "parse ! va" + codec +
      "dec ! video/x-raw,format=" + (ten_bit ? "P010_10LE" : "NV12") +
      " ! appsink name=result sync=false emit-signals=true";
  auto* pipeline = gst_parse_launch(graph.c_str(), &error);
  Check(pipeline && !error, error ? error->message : "cannot create encode pipeline");
  EncodedChecks encoded{ten_bit};
  auto* encoder = gst_bin_get_by_name(GST_BIN(pipeline), "encoder");
  auto* encoded_pad = gst_element_get_static_pad(encoder, "src");
  gst_pad_add_probe(
      encoded_pad,
      GST_PAD_PROBE_TYPE_BUFFER,
      [](GstPad*, GstPadProbeInfo* info, gpointer data) {
        static_cast<EncodedChecks*>(data)->Inspect(GST_PAD_PROBE_INFO_BUFFER(info), false);
        return GST_PAD_PROBE_OK;
      },
      &encoded,
      nullptr);
  const auto filter_status = hm::program::InstallEncodedColorimetry(
      encoder, ten_bit ? hm::program::EncodedCodec::kHevc : hm::program::EncodedCodec::kH264, info, 1024 * 1024);
  Check(filter_status.ok(), filter_status.ToString());
  gst_pad_add_probe(
      encoded_pad,
      GST_PAD_PROBE_TYPE_BUFFER,
      [](GstPad*, GstPadProbeInfo* info, gpointer data) {
        static_cast<EncodedChecks*>(data)->Inspect(GST_PAD_PROBE_INFO_BUFFER(info), true);
        return GST_PAD_PROBE_OK;
      },
      &encoded,
      nullptr);
  gst_object_unref(encoded_pad);
  gst_object_unref(encoder);
  pool->SetDisplayContext(pipeline);
  auto* input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
  auto* result = gst_bin_get_by_name(GST_BIN(pipeline), "result");
  gst_app_src_set_caps(GST_APP_SRC(input), pool->caps());
  Results results;
  g_signal_connect(result, "new-sample", G_CALLBACK(Read), &results);
  Check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "encode startup failed");
  hipStream_t stream{};
  Hip(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking));
  std::vector<uint8_t> pixels(768 * 192 * 4, 0);
  for (int y = 0; y < 192; ++y)
    for (int x = 0; x < 768; ++x) {
      pixels[(y * 768 + x) * 4 + (x < 384 ? 0 : 2)] = 255;
      pixels[(y * 768 + x) * 4 + 3] = 255;
    }
  void* source{};
  Hip(hipMalloc(&source, pixels.size()));
  Hip(hipMemcpyAsync(source, pixels.data(), pixels.size(), hipMemcpyHostToDevice, stream));
  const hm::gpu::Image image{source, nullptr, 768 * 4, 0, 768, 192, hm::gpu::PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  for (unsigned sequence = 0; sequence < 32; ++sequence) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto acquired = pool->Acquire();
    while (absl::IsResourceExhausted(acquired.status()) && std::chrono::steady_clock::now() < deadline) {
      auto* bus = gst_element_get_bus(pipeline);
      auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
      gst_object_unref(bus);
      if (message) {
        gst_message_parse_error(message, &error, nullptr);
        Check(false, error->message);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      acquired = pool->Acquire();
    }
    Check(acquired.ok(), acquired.status().ToString());
    auto* buffer = *acquired;
    auto imported = hm::gpu::VaHipFrame::Import(buffer, stream, hm::gpu::VaHipFrame::Access::kWrite);
    Check(imported.ok(), imported.status().ToString());
    auto frame = std::move(*imported);
    const auto output = hm::playcropper::DescribeVaProgramOutput(*frame, info);
    Check(output.ok(), output.status().ToString());
    const float left = sequence % 2 == 0 ? 0 : 384;
    const hm::playcropper::FrameTransformGeometry transform{
        hm::BBox(0, 0, 768, 192), hm::Point{0, 0}, hm::BBox(left, 0, left + 384, 192), 0};
    Hip(hm::playcropper::RenderProgramYuv(image, *output, transform, 0, false, 0, stream));
    // Exercise both explicit fence and early-return destructor fence.
    if (sequence % 2)
      Check(frame->Complete().ok(), "output stream failed");
    frame.reset();
    GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(sequence, GST_SECOND, 25);
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(sequence + 1, GST_SECOND, 25) - GST_BUFFER_PTS(buffer);
    Check(gst_app_src_push_buffer(GST_APP_SRC(input), buffer) == GST_FLOW_OK, "encode input refused frame");
  }
  Check(gst_app_src_end_of_stream(GST_APP_SRC(input)) == GST_FLOW_OK, "encode EOS refused");
  auto* bus = gst_element_get_bus(pipeline);
  auto* message = gst_bus_timed_pop_filtered(
      bus, 30 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    gchar* debug{};
    gst_message_parse_error(message, &error, &debug);
    std::cerr << error->message << ": " << (debug ? debug : "") << '\n';
    g_error_free(error);
    g_free(debug);
  }
  Check(message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS, "encode output did not reach EOS");
  Check(results.frames == 32, "encode dropped frames");
  Check(encoded.packets == 32 && encoded.headers > 0, "Missing encoded colorimetry verification");
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_message_unref(message);
  gst_object_unref(bus);
  gst_object_unref(input);
  gst_object_unref(result);
  gst_object_unref(pipeline);
  Hip(hipStreamSynchronize(stream));
  Hip(hipFree(source));
  Hip(hipStreamDestroy(stream));
  // Outstanding surfaces retain their VA allocation after their pool owner dies.
  auto last = pool->Acquire();
  Check(last.ok(), last.status().ToString());
  pool.reset();
  Hip(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking));
  auto retained = hm::gpu::VaHipFrame::Import(*last, stream, hm::gpu::VaHipFrame::Access::kRead);
  Check(retained.ok(), retained.status().ToString());
  Check((*retained)->Complete().ok(), "retained output completion");
  retained->reset();
  gst_buffer_unref(*last);
  Hip(hipStreamDestroy(stream));
  std::cout << codec << ": 32 Program crops, bounded pool, encoded YUV pixels, timestamps and EOS passed\n";
}
