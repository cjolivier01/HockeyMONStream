#include "hstream/src/libs/gpu/vaapi/VaVideoReader.h"

#include <unistd.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "hstream/src/libs/gpu/vaapi/VaHipFrame.h"

namespace {
void Check(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
void Status(const absl::Status& status) {
  Check(status.ok(), status.ToString());
}
template <typename T>
T Take(absl::StatusOr<T> result) {
  Status(result.status());
  return std::move(*result);
}
struct Pipeline {
  GstElement* value = nullptr;
  ~Pipeline() {
    if (value) {
      gst_element_set_state(value, GST_STATE_NULL);
      gst_object_unref(value);
    }
  }
};
void MakeVideo(const std::filesystem::path& path, bool white, bool hevc) {
  const std::string graph = std::string("videotestsrc num-buffers=20 pattern=") + (white ? "white" : "black") +
      " ! video/x-raw,format=" + (hevc ? "P010_10LE" : "NV12") +
      ",width=384,height=192,framerate=25/1,colorimetry=bt709 "
      "! vapostproc ! " +
      (hevc ? "vah265enc b-frames=0 ! h265parse" : "vah264enc b-frames=0 ! h264parse") +
      " ! mp4mux ! filesink name=output";
  GError* error = nullptr;
  Pipeline pipeline{gst_parse_launch(graph.c_str(), &error)};
  const std::string detail = error ? error->message : "Cannot construct video";
  g_clear_error(&error);
  Check(pipeline.value != nullptr, detail);
  auto* output = gst_bin_get_by_name(GST_BIN(pipeline.value), "output");
  g_object_set(output, "location", path.c_str(), nullptr);
  gst_object_unref(output);
  Check(gst_element_set_state(pipeline.value, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Cannot start encoder");
  auto* bus = gst_element_get_bus(pipeline.value);
  auto* message = gst_bus_timed_pop_filtered(
      bus, 20 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  gst_object_unref(bus);
  const bool eos = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
  if (message)
    gst_message_unref(message);
  Check(eos, "Cannot encode synthetic chapter");
}

struct Stream {
  hipStream_t value{};
  Stream() {
    Check(hipStreamCreate(&value) == hipSuccess, "Cannot create HIP stream");
  }
  ~Stream() {
    (void)hipStreamDestroy(value);
  }
};
void Pixel(const hm::gpu::VaVideoFrame& frame, bool white, hipStream_t stream) {
  auto imported = Take(hm::gpu::VaHipFrame::Import(
      gst_sample_get_buffer(frame.sample.get()), stream, hm::gpu::VaHipFrame::Access::kRead));
  unsigned short pixel = 0;
  const bool p010 = imported->format() == hm::gpu::VaHipFrame::Format::kP010;
  // Only a two-byte synthetic test sample is read back, never production video.
  Check(
      hipMemcpyAsync(&pixel, imported->plane(0).data, p010 ? 2 : 1, hipMemcpyDeviceToHost, stream) == hipSuccess,
      "Cannot inspect synthetic pixel");
  Status(imported->Complete());
  const int luma = p010 ? pixel >> 8 : pixel;
  Check(std::abs(luma - (white ? 235 : 16)) <= 2, "Retained frame pixels changed");
}
hm::gpu::VaVideoFrame Frame(hm::gpu::VaVideoReader& reader, int index) {
  auto result = Take(reader.Read());
  Check(result.has_value(), "Premature EOS");
  Check(
      result->time == GstClockTime(index) * 40 * GST_MSECOND && result->duration == 40 * GST_MSECOND,
      "Incorrect file-relative timestamp after seek/chapter switch: " + std::to_string(result->time));
  return std::move(*result);
}
size_t Descriptors() {
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
    (void)entry, ++count;
  return count;
}
void Exercise(const std::filesystem::path& root, bool hevc, const std::string& render) {
  std::cout << (hevc ? "HEVC" : "H264") << " creating inputs\n";
  const auto black = root / (hevc ? "black-hevc.mp4" : "black-h264.mp4");
  const auto white = root / (hevc ? "white-hevc.mp4" : "white-h264.mp4");
  MakeVideo(black, false, hevc);
  MakeVideo(white, true, hevc);
  hm::gpu::VaVideoReaderConfig config{
      hevc ? hm::gpu::VaVideoCodec::kHevc : hm::gpu::VaVideoCodec::kH264,
      hevc ? GST_VIDEO_FORMAT_P010_10LE : GST_VIDEO_FORMAT_NV12,
      render,
      2,
      10 * GST_SECOND,
      10 * GST_MSECOND};
  Stream stream;
  auto reader = Take(hm::gpu::VaVideoReader::Create(config));
  std::cout << "Opening black\n";
  Status(reader->OpenFile(black, 0));
  auto retained = Frame(*reader, 0);
  for (int i = 1; i != 20; ++i)
    (void)Frame(*reader, i);
  Check(!Take(reader->Read()).has_value(), "Missing clean EOS");
  std::cout << "Seek after EOS\n";
  Status(reader->Seek(240 * GST_MSECOND));
  Pixel(Frame(*reader, 6), false, stream.value);
  for (int i = 7; i != 20; ++i)
    (void)Frame(*reader, i);
  Check(!Take(reader->Read()).has_value(), "Missing EOS after seek");
  std::cout << "Opening white\n";
  Status(reader->OpenFile(white, 120 * GST_MSECOND));
  Pixel(Frame(*reader, 3), true, stream.value);
  std::cout << "Reading retained black\n";
  Pixel(retained, false, stream.value);
  // An active appsink queue must flush without losing the retained old surface.
  Status(reader->Seek(0));
  Pixel(Frame(*reader, 0), true, stream.value);
  // Warm the decoder pools before checking repeated flush/reuse behavior.
  Status(reader->Seek(400 * GST_MSECOND));
  (void)Frame(*reader, 10);
  const auto before = Descriptors();
  std::cout << "Repeated seek\n";
  for (int i = 0; i != 16; ++i) {
    Status(reader->Seek((i % 10) * 40 * GST_MSECOND));
    Pixel(Frame(*reader, i % 10), true, stream.value);
  }
  const auto after = Descriptors();
  Check(
      after <= before + 4,
      "Repeated seek leaked descriptors: " + std::to_string(before) + " -> " + std::to_string(after));
  for (int i = 0; i != 8; ++i) {
    Status(reader->OpenFile(i % 2 ? white : black, 0));
    Pixel(Frame(*reader, 0), i % 2, stream.value);
  }
  const auto chapters = Descriptors();
  std::cout << "Chapter descriptors " << after << " -> " << chapters << '\n';
  Check(
      chapters <= after + 4,
      "Chapter changes leaked descriptors; use the fixed private VA runtime on GStreamer 1.24.2");
  std::thread cancel([&] { reader->Cancel(); });
  cancel.join();
  Check(absl::IsCancelled(reader->Read().status()), "Cancellation became EOS");
  Check(absl::IsCancelled(reader->Seek(0)), "Cancelled reader accepted a seek");
  reader.reset();
  Pixel(retained, false, stream.value);

  auto invalid = config;
  invalid.maximum_queued_frames = 0;
  Check(!hm::gpu::VaVideoReader::Create(invalid).ok(), "Unbounded queue accepted");
  invalid = config;
  invalid.operation_timeout = GST_CLOCK_TIME_NONE;
  Check(!hm::gpu::VaVideoReader::Create(invalid).ok(), "Infinite timeout accepted");
  invalid = config;
  invalid.render_node = "/dev/null";
  Check(!hm::gpu::VaVideoReader::Create(invalid).ok(), "Wrong device accepted");
  reader = Take(hm::gpu::VaVideoReader::Create(config));
  Status(reader->OpenFile(black, 0));
  Check(!reader->OpenFile(root / "missing.mp4", 0).ok(), "Missing chapter accepted");
  Check(!reader->Read().ok(), "Reader continued after a failed transition");
  std::cout << (hevc ? "HEVC/P010" : "H264/NV12")
            << " EOF, seek, chapter, retained frame, cancel and failure checks passed; fds " << before << " -> "
            << after << '\n';
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  std::cout << std::unitbuf;
  char temporary[] = "/tmp/hstream-va-reader-XXXXXX";
  const auto* directory = mkdtemp(temporary);
  if (!directory)
    return 1;
  int result = 0;
  try {
    char pci[32]{};
    Check(hipDeviceGetPCIBusId(pci, sizeof(pci), 0) == hipSuccess, "Missing HIP device");
    const std::string render = std::string("/dev/dri/by-path/pci-") + pci + "-render";
    Exercise(directory, false, render);
    Exercise(directory, true, render);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    result = 1;
  }
  std::filesystem::remove_all(directory);
  return result;
}
