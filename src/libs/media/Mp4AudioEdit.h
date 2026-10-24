#pragma once

#include <cstdint>

#include "absl/status/status.h"

namespace hm::media {
// Finalize completed mp4mux/qtmux AAC-LC output before publication. Retain its
// 1024-sample avenc_aac preroll, but remove priming/padding from the audible
// edit. All metadata is validated before positional writes; video/media payload
// and descriptor offset remain unchanged. Caller owns exclusive access/fsync.
absl::Status FinalizeMp4Audio(int fd, int sample_rate, int64_t samples);
} // namespace hm::media
