#include "hstream/src/libs/gpu/vaapi/VaOutputPool.h"
#include "hstream/src/libs/program/VaProgramOutput.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <fcntl.h>
#include <gbm.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>
#include <unistd.h>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void Check(bool ok, const std::string& message) {
  if (!ok) {
    std::cerr << message << std::endl;
    std::abort();
  }
}
void Hip(hipError_t error) {
  Check(error == hipSuccess, hipGetErrorString(error));
}
void Inspect(GstGLContext* context, gpointer data) {
  auto* memory = static_cast<GstGLMemory*>(data);
  auto* gl = context->gl_vtable;
  Check(memory->tex_target == GST_GL_TEXTURE_TARGET_2D, "expected 2D preview texture");
  GLuint framebuffer{};
  gl->GenFramebuffers(1, &framebuffer);
  gl->BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, memory->tex_id, 0);
  Check(gl->CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "preview framebuffer incomplete");
  // Four test-only bytes from a synthetic frame after actual GL upload/conversion.
  unsigned char pixel[4]{};
  gl->ReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  Check(gl->GetError() == GL_NO_ERROR, "preview GL read failed");
  Check(pixel[0] > 220 && pixel[1] < 20 && pixel[2] < 20, "GL preview color mismatch");
  std::cout << "VA/HIP Program preview rendered on " << gl->GetString(GL_RENDERER) << '\n';
  gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
  gl->DeleteFramebuffers(1, &framebuffer);
  gl->Finish();
}
} // namespace
int main(int argc, char** argv) {
  const bool ten_bit = argc == 2 && std::string(argv[1]) == "--p010";
  // The qualifier is headless. Production desktop selects its own window.
  g_setenv("GST_GL_WINDOW", "surfaceless", TRUE);
  g_setenv("GST_GL_PLATFORM", "egl", TRUE);
  g_setenv("GST_GL_API", "gles2", TRUE);
  gst_init(&argc, &argv);
  Hip(hipSetDevice(0));
  char pci[32]{};
  Hip(hipDeviceGetPCIBusId(pci, sizeof(pci), 0));
  const std::string path = std::string("/dev/dri/by-path/pci-") + pci + "-render";
  const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  Check(fd >= 0, "missing HIP render node");
  auto* gbm = gbm_create_device(fd);
  Check(gbm, "GBM device failed");
  auto egl = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
  EGLint major, minor;
  Check(eglInitialize(egl, &major, &minor), "EGL initialization failed");
  auto* display = gst_gl_display_egl_new_with_egl_display(egl);
  Check(display, "GL display wrapper failed");
  auto* context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
  gst_context_set_gl_display(context, GST_GL_DISPLAY(display));

  GstVideoInfo info{};
  Check(
      gst_video_info_set_format(&info, ten_bit ? GST_VIDEO_FORMAT_P010_10LE : GST_VIDEO_FORMAT_NV12, 128, 64),
      "video format");
  info.fps_n = 25;
  info.fps_d = 1;
  info.chroma_site = GST_VIDEO_CHROMA_SITE_JPEG;
  Check(gst_video_colorimetry_from_string(&info.colorimetry, "bt709"), "video colorimetry");
  auto created = hm::gpu::VaOutputPool::Create(0, info, 2);
  Check(created.ok(), created.status().ToString());
  auto pool = std::move(*created);
  GError* error{};
  auto* pipeline = gst_parse_launch(
      "appsrc name=input format=time ! glupload ! glcolorconvert ! "
      "video/x-raw(memory:GLMemory),format=RGBA,texture-target=2D ! appsink name=result sync=false",
      &error);
  Check(pipeline && !error, error ? error->message : "GL pipeline creation failed");
  gst_element_set_context(pipeline, context);
  gst_context_unref(context);
  pool->SetDisplayContext(pipeline);
  auto* input = gst_bin_get_by_name(GST_BIN(pipeline), "input");
  auto* result = gst_bin_get_by_name(GST_BIN(pipeline), "result");
  gst_app_src_set_caps(GST_APP_SRC(input), pool->caps());
  Check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "GL pipeline startup failed");
  hipStream_t stream{};
  Hip(hipStreamCreateWithFlags(&stream, hipStreamNonBlocking));
  // One 2x2 synthetic RGBA tile, uploaded only by this test.
  const uint8_t pixels[] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
  void* source{};
  Hip(hipMalloc(&source, sizeof(pixels)));
  Hip(hipMemcpyAsync(source, pixels, sizeof(pixels), hipMemcpyHostToDevice, stream));
  auto acquired = pool->Acquire();
  Check(acquired.ok(), acquired.status().ToString());
  auto* buffer = *acquired;
  auto imported = hm::gpu::VaHipFrame::Import(buffer, stream, hm::gpu::VaHipFrame::Access::kWrite);
  Check(imported.ok(), imported.status().ToString());
  auto output = hm::playcropper::DescribeVaProgramOutput(**imported, info);
  Check(output.ok(), output.status().ToString());
  const hm::gpu::Image image{source, nullptr, 8, 0, 2, 2, hm::gpu::PixelFormat::kRgba8, 0, 0, 0, 0, 0, 0, 0};
  const hm::playcropper::FrameTransformGeometry t{hm::BBox(0, 0, 2, 2), hm::Point{0, 0}, hm::BBox(0, 0, 1, 1), 0};
  Hip(hm::playcropper::RenderProgramYuv(image, *output, t, 0, false, 0, stream));
  Check((*imported)->Complete().ok(), "HIP completion failed");
  imported->reset();
  GST_BUFFER_PTS(buffer) = 0;
  GST_BUFFER_DURATION(buffer) = GST_SECOND / 25;
  Check(gst_app_src_push_buffer(GST_APP_SRC(input), buffer) == GST_FLOW_OK, "GL input refused frame");
  Check(gst_app_src_end_of_stream(GST_APP_SRC(input)) == GST_FLOW_OK, "GL EOS refused");
  auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(result), 30 * GST_SECOND);
  Check(sample, "GL output missing");
  auto* memory = gst_buffer_peek_memory(gst_sample_get_buffer(sample), 0);
  Check(gst_is_gl_memory(memory), "preview unexpectedly returned system memory");
  auto* glmemory = reinterpret_cast<GstGLMemory*>(memory);
  gst_gl_context_thread_add(glmemory->mem.context, Inspect, glmemory);
  gst_sample_unref(sample);
  auto* bus = gst_element_get_bus(pipeline);
  auto* message = gst_bus_timed_pop_filtered(
      bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  Check(message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS, "GL output did not reach EOS");
  gst_message_unref(message);
  gst_object_unref(bus);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(input);
  gst_object_unref(result);
  gst_object_unref(pipeline);
  Hip(hipFree(source));
  Hip(hipStreamDestroy(stream));
  pool.reset();
  gst_object_unref(display);
  eglTerminate(egl);
  gbm_device_destroy(gbm);
  close(fd);
}
