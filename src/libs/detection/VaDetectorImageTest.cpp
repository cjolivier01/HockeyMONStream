// Include HIP before GLib too: SDK macros must not break system headers.
#include "hstream/src/libs/gpu/Runtime.h"

#include "hstream/src/libs/detection/VaDetectorImage.h"

#include <gst/app/gstappsink.h>
#include <gst/video/video-info-dma.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const std::string& error) {
  if (!value)
    throw std::runtime_error(error);
}
struct Resources {
  hipStream_t stream{};
  GstElement* pipeline{};
  GstElement* sink{};
  GstSample* sample{};
  ~Resources() {
    if (stream)
      (void)hipStreamSynchronize(stream);
    if (sample)
      gst_sample_unref(sample);
    if (pipeline)
      gst_element_set_state(pipeline, GST_STATE_NULL);
    if (sink)
      gst_object_unref(sink);
    if (pipeline)
      gst_object_unref(pipeline);
    if (stream)
      (void)hipStreamDestroy(stream);
  }
};
void Test(bool ten_bit) {
  Resources resources;
  Check(hipStreamCreateWithFlags(&resources.stream, hipStreamNonBlocking) == hipSuccess, "create stream");
  const std::string format = ten_bit ? "P010_10LE" : "NV12";
  const std::string drm = ten_bit ? "P010" : "NV12";
  const std::string graph = "videotestsrc num-buffers=1 ! video/x-raw,format=" + format +
      ",width=64,height=48,colorimetry=bt709 ! vapostproc disable-passthrough=true ! "
      "video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=" +
      drm + " ! appsink name=result sync=false";
  GError* error = nullptr;
  resources.pipeline = gst_parse_launch(graph.c_str(), &error);
  if (error) {
    const std::string message = error->message;
    g_error_free(error);
    throw std::runtime_error(message);
  }
  Check(resources.pipeline, "create VA graph");
  resources.sink = gst_bin_get_by_name(GST_BIN(resources.pipeline), "result");
  GstAppSinkCallbacks callbacks{};
  callbacks.propose_allocation = [](GstAppSink*, GstQuery* query, gpointer) -> gboolean {
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
    return TRUE;
  };
  gst_app_sink_set_callbacks(GST_APP_SINK(resources.sink), &callbacks, nullptr, nullptr);
  Check(gst_element_set_state(resources.pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "start VA graph");
  resources.sample = gst_app_sink_try_pull_sample(GST_APP_SINK(resources.sink), 20 * GST_SECOND);
  Check(resources.sample, "receive VA frame");
  GstVideoInfoDmaDrm info{};
  Check(gst_video_info_dma_drm_from_caps(&info, gst_sample_get_caps(resources.sample)), "parse VA caps");
  auto frame = hm::gpu::VaHipFrame::Import(
      gst_sample_get_buffer(resources.sample), resources.stream, hm::gpu::VaHipFrame::Access::kRead);
  Check(frame.ok(), frame.status().ToString());
  auto describe = [&](const GstVideoInfo& value) { return hm::detection::DescribeVaDetectorImage(**frame, value); };
  auto image = describe(info.vinfo);
  Check(image.ok(), image.status().ToString());
  Check(
      image->plane0 == (*frame)->plane(0).data && image->plane1 == (*frame)->plane(1).data,
      "describe borrowed planes without copies");
  const float code_max = ten_bit ? 1023 : 255;
  const float factor = ten_bit ? 4 : 1;
  Check(std::abs(image->y_scale * 219 * factor / code_max - 1) < 1e-6, "limited-range luma normalization");
  Check(std::abs(image->r_v * 224 * factor / code_max - 1.5748f) < 1e-5, "BT.709 red coefficient");
  Check(std::abs(image->b_u * 224 * factor / code_max - 1.8556f) < 1e-5, "BT.709 blue coefficient");
  auto changed = info.vinfo;
  changed.colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;
  image = describe(changed);
  Check(image.ok() && image->y_offset == 0 && image->y_scale == 1, "full-range normalization");
  changed = info.vinfo;
  changed.width++;
  Check(!describe(changed).ok(), "reject changed dimensions");
  changed = info.vinfo;
  changed.finfo = gst_video_format_get_info(ten_bit ? GST_VIDEO_FORMAT_NV12 : GST_VIDEO_FORMAT_P010_10LE);
  Check(!describe(changed).ok(), "reject changed pixel layout");
  changed = info.vinfo;
  changed.colorimetry.range = GST_VIDEO_COLOR_RANGE_UNKNOWN;
  Check(!describe(changed).ok(), "reject missing YUV range");
  changed = info.vinfo;
  changed.colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_UNKNOWN;
  Check(!describe(changed).ok(), "reject missing YUV matrix");
  changed = info.vinfo;
  changed.colorimetry.transfer = GST_VIDEO_TRANSFER_SMPTE2084;
  Check(!describe(changed).ok(), "PQ requires tone mapping");
  changed.colorimetry.transfer = GST_VIDEO_TRANSFER_ARIB_STD_B67;
  Check(!describe(changed).ok(), "HLG requires tone mapping");
  Check((*frame)->Complete().ok(), "complete import");
}
} // namespace
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    Test(false);
    Test(true);
    std::cout << "VA detector colorimetry and caps checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
