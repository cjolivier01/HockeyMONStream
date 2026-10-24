#include "hstream/src/libs/program/EncodedColorimetry.h"

#include <cstring>
#include <limits>
#include <memory>
#include <string>
extern "C" {
#include <libavcodec/bsf.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace hm::program {
namespace {
struct Filter {
  AVBSFContext* context{};
  size_t maximum_bytes;
  EncodedCodec codec;
  bool retired{false};
  ~Filter() {
    av_bsf_free(&context);
  }
};
std::string Error(int code) {
  char message[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(code, message, sizeof(message));
  return message;
}
void Fail(GstPad* pad, Filter& filter, const char* text) {
  filter.retired = true;
  auto* owner = gst_pad_get_parent_element(pad);
  if (!owner)
    return;
  GError* error = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT, text);
  gst_element_post_message(owner, gst_message_new_error(GST_OBJECT(owner), error, "Encoded colorimetry"));
  g_error_free(error);
  gst_object_unref(owner);
}
void Fail(GstPad* pad, Filter& filter, const std::string& text) {
  Fail(pad, filter, text.c_str());
}
GstPadProbeReturn RewriteImpl(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
  auto& filter = *static_cast<Filter*>(data);
  if (filter.retired)
    return GST_PAD_PROBE_DROP;
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    auto* event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
      GstCaps* caps{};
      gst_event_parse_caps(event, &caps);
      if (!caps || gst_caps_get_size(caps) != 1 || !gst_caps_is_fixed(caps)) {
        Fail(pad, filter, "Encoded colorimetry requires fixed access-unit caps");
        return GST_PAD_PROBE_DROP;
      }
      const auto* structure = gst_caps_get_structure(caps, 0);
      const char* expected = filter.codec == EncodedCodec::kH264 ? "video/x-h264" : "video/x-h265";
      if (!gst_structure_has_name(structure, expected) ||
          g_strcmp0(gst_structure_get_string(structure, "stream-format"), "byte-stream") ||
          g_strcmp0(gst_structure_get_string(structure, "alignment"), "au")) {
        Fail(pad, filter, "Encoded colorimetry requires Annex B access units");
        return GST_PAD_PROBE_DROP;
      }
    }
    return GST_PAD_PROBE_OK;
  }
  auto* input = GST_PAD_PROBE_INFO_BUFFER(info);
  const auto bytes = gst_buffer_get_size(input);
  if (!bytes || bytes > filter.maximum_bytes) {
    Fail(pad, filter, "Encoded packet exceeds the configured colorimetry bound");
    return GST_PAD_PROBE_DROP;
  }
  std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
  if (!packet || av_new_packet(packet.get(), static_cast<int>(bytes)) < 0) {
    Fail(pad, filter, "Cannot allocate bounded encoded packet");
    return GST_PAD_PROBE_DROP;
  }
  // VA encoders already publish compressed bytes in host memory. This copy
  // supplies libavcodec's required input padding, never decoded video pixels.
  if (gst_buffer_extract(input, 0, packet->data, bytes) != bytes) {
    Fail(pad, filter, "Cannot read compressed access unit");
    return GST_PAD_PROBE_DROP;
  }
  int status = av_bsf_send_packet(filter.context, packet.get());
  if (status >= 0)
    status = av_bsf_receive_packet(filter.context, packet.get());
  if (status < 0 || packet->size <= 0 || static_cast<size_t>(packet->size) > filter.maximum_bytes) {
    Fail(pad, filter, "Cannot preserve encoded colorimetry: " + Error(status < 0 ? status : AVERROR(EINVAL)));
    return GST_PAD_PROBE_DROP;
  }
  // The metadata filters return exactly one packet per submitted access unit.
  // Let GstMemory own the returned packet without a second payload copy.
  auto* output = gst_buffer_new_wrapped_full(
      GST_MEMORY_FLAG_READONLY, packet->data, packet->size, 0, packet->size, packet.get(), [](gpointer p) {
        auto* packet = static_cast<AVPacket*>(p);
        av_packet_free(&packet);
      });
  if (!output) {
    Fail(pad, filter, "Cannot wrap encoded colorimetry result");
    return GST_PAD_PROBE_DROP;
  }
  (void)packet.release();
  if (!gst_buffer_copy_into(output, input, GST_BUFFER_COPY_METADATA, 0, -1)) {
    gst_buffer_unref(output);
    Fail(pad, filter, "Cannot preserve encoded buffer metadata");
    return GST_PAD_PROBE_DROP;
  }
  GST_PAD_PROBE_INFO_DATA(info) = output;
  gst_buffer_unref(input);
  return GST_PAD_PROBE_OK;
}
GstPadProbeReturn Rewrite(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
  try {
    return RewriteImpl(pad, info, data);
  } catch (...) {
    // Never unwind a C++ exception through GStreamer's streaming thread.
    Fail(pad, *static_cast<Filter*>(data), "Encoded colorimetry processing failed");
    return GST_PAD_PROBE_DROP;
  }
}
} // namespace

absl::Status InstallEncodedColorimetry(
    GstElement* encoder,
    EncodedCodec codec,
    const GstVideoInfo& input,
    size_t maximum_packet_bytes) {
  if (!encoder || !input.finfo || input.width <= 0 || input.height <= 0 ||
      (GST_VIDEO_INFO_FORMAT(&input) != GST_VIDEO_FORMAT_NV12 &&
       GST_VIDEO_INFO_FORMAT(&input) != GST_VIDEO_FORMAT_P010_10LE) ||
      !maximum_packet_bytes || maximum_packet_bytes > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      (codec != EncodedCodec::kH264 && codec != EncodedCodec::kHevc) ||
      input.chroma_site != GST_VIDEO_CHROMA_SITE_JPEG || input.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_UNKNOWN ||
      input.colorimetry.primaries == GST_VIDEO_COLOR_PRIMARIES_UNKNOWN ||
      input.colorimetry.transfer == GST_VIDEO_TRANSFER_UNKNOWN ||
      (input.colorimetry.range != GST_VIDEO_COLOR_RANGE_0_255 &&
       input.colorimetry.range != GST_VIDEO_COLOR_RANGE_16_235))
    return absl::InvalidArgumentError("Encoded colorimetry requires explicit color, centered chroma and packet bound");
  if (gst_video_color_primaries_to_iso(input.colorimetry.primaries) == 2 ||
      gst_video_transfer_function_to_iso(input.colorimetry.transfer) == 2 ||
      gst_video_color_matrix_to_iso(input.colorimetry.matrix) == 2)
    return absl::InvalidArgumentError("Encoded colorimetry must have explicit ISO color identifiers");
  const auto* implementation = av_bsf_get_by_name(codec == EncodedCodec::kH264 ? "h264_metadata" : "hevc_metadata");
  if (!implementation)
    return absl::UnavailableError("libavcodec lacks the required metadata bitstream filter");
  auto filter = std::make_unique<Filter>();
  int status = av_bsf_alloc(implementation, &filter->context);
  if (status < 0)
    return absl::ResourceExhaustedError(Error(status));
  filter->maximum_bytes = maximum_packet_bytes;
  filter->codec = codec;
  filter->context->par_in->codec_id = codec == EncodedCodec::kH264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
  filter->context->time_base_in = AVRational{1, 1}; // GStreamer retains the original timestamps independently.
  const struct {
    const char* name;
    int64_t value;
  } options[] = {
      {"video_full_range_flag", input.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255},
      {"colour_primaries", gst_video_color_primaries_to_iso(input.colorimetry.primaries)},
      {"transfer_characteristics", gst_video_transfer_function_to_iso(input.colorimetry.transfer)},
      {"matrix_coefficients", gst_video_color_matrix_to_iso(input.colorimetry.matrix)},
      {"chroma_sample_loc_type", 1}, // centered 4:2:0, matching RenderProgramYuv.
  };
  for (const auto& option : options) {
    status = av_opt_set_int(filter->context->priv_data, option.name, option.value, 0);
    if (status < 0)
      return absl::InvalidArgumentError(std::string(option.name) + ": " + Error(status));
  }
  status = av_bsf_init(filter->context);
  if (status < 0)
    return absl::FailedPreconditionError(Error(status));
  auto* pad = gst_element_get_static_pad(encoder, "src");
  if (!pad)
    return absl::InvalidArgumentError("Encoder has no static source pad");
  const auto id = gst_pad_add_probe(
      pad,
      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
      Rewrite,
      filter.get(),
      [](gpointer value) { delete static_cast<Filter*>(value); });
  gst_object_unref(pad);
  if (!id)
    return absl::InternalError("Could not attach encoded colorimetry filter");
  (void)filter.release();
  return absl::OkStatus();
}
} // namespace hm::program
