#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "absl/status/statusor.h"

namespace hm::media {
struct Mp4AudioReaderConfig {
  int64_t operation_timeout_ns;
  size_t maximum_packet_bytes, maximum_frame_samples;
};
struct AudioFormat {
  int rate, channels;
};
struct AudioFrame {
  // Absolute file timestamp in sample-rate ticks, after MP4 edits and codec
  // priming. Samples are owned, interleaved float PCM. No video is decoded.
  int64_t sample;
  AudioFormat format;
  std::vector<float> pcm;
  size_t samples() const {
    return pcm.size() / format.channels;
  }
};

// A bounded native AAC-LC reader for ordinary local MP4 camera recordings.
// One serialized owner, except Cancel(). Open/Seek/Read failures retire it;
// argument errors before changing state do not. Returned PCM outlives it.
// Seek positions in the file clock and drops complete preceding frames; its
// first result may straddle the requested sample for sample-accurate trimming.
class Mp4AudioReader {
 public:
  static absl::StatusOr<std::unique_ptr<Mp4AudioReader>> Create(const Mp4AudioReaderConfig& config);
  ~Mp4AudioReader();
  absl::Status Open(const std::filesystem::path& file);
  absl::Status Seek(int64_t sample);
  absl::StatusOr<std::optional<AudioFrame>> Read();
  void Cancel() noexcept;
  AudioFormat format() const;

 private:
  struct Impl;
  explicit Mp4AudioReader(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace hm::media
