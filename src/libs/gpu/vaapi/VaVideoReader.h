#pragma once

#include <gst/gst.h>
#include <gst/video/video-info.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>

#include "absl/status/statusor.h"

namespace hm::gpu {

enum class VaVideoCodec { kH264, kHevc };

// Values are resolved by the application. No device, queue or deadline defaults
// are selected here. The render node must identify the processing GPU.
struct VaVideoReaderConfig {
  VaVideoCodec codec;
  GstVideoFormat format;
  std::filesystem::path render_node;
  unsigned maximum_queued_frames;
  GstClockTime operation_timeout;
  GstClockTime cancellation_poll_interval;
};

struct VaVideoFrame {
  std::unique_ptr<GstSample, decltype(&gst_sample_unref)> sample{nullptr, gst_sample_unref};
  GstVideoInfo info{};
  // File-relative stream time, independent of the seek's running-time origin.
  GstClockTime time = GST_CLOCK_TIME_NONE;
  GstClockTime duration = GST_CLOCK_TIME_NONE;
};

// Local MP4 video reader with explicitly selected VA decoding and linear
// DMA-BUF output. It never maps video and never falls back to a CPU decoder.
// OpenFile changes chapters using the same elements; Seek flushes the current
// file without replacing the decoder. A returned sample owns its surface even
// across seeks, chapter changes and reader destruction.
//
// Serialize every method and destruction on one owner thread, except Cancel,
// which may run concurrently. Cancel is permanent; recreate for a new run.
// Waits poll for cancellation; driver calls themselves are not interruptible.
// Errors retire the reader, preventing continued use of a partial transition.
class VaVideoReader {
 public:
  static absl::StatusOr<std::unique_ptr<VaVideoReader>> Create(const VaVideoReaderConfig& config);
  ~VaVideoReader();
  VaVideoReader(const VaVideoReader&) = delete;
  VaVideoReader& operator=(const VaVideoReader&) = delete;

  absl::Status OpenFile(const std::filesystem::path& path, GstClockTime start);
  absl::Status Seek(GstClockTime time);
  // An empty optional is clean EOS. Cancellation, decoding failures and stalls
  // remain distinct errors. EOS may be followed by Seek or OpenFile.
  absl::StatusOr<std::optional<VaVideoFrame>> Read();
  void Cancel() noexcept;

 private:
  struct Impl;
  explicit VaVideoReader(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::gpu
