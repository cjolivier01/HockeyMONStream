#pragma once

#include <memory>

#include "absl/status/statusor.h"
#include "hstream/src/libs/gpu/Image.h"
#include "hstream/src/libs/gpu/Runtime.h"
#include "hstream/src/libs/scoreboard/CameraDecoder.h"

namespace hm::scoreboard {
// A bounded pool of compact, immutable GPU scoreboard images. The decoder
// worker copies into this pool so retaining an overlay never retains a decoder
// surface. Exhaustion applies backpressure; it never overwrites a live image.
class CameraImages {
 public:
  static absl::StatusOr<std::unique_ptr<CameraImages>> Create(
      unsigned device,
      int width,
      int height,
      unsigned capacity);
  ~CameraImages();
  // Serialized worker operation. Waits only on this pool's independent GPU
  // stream, then releases the imported decoder surface before returning. No
  // pixel readback. ResourceExhausted means retry this same decoded frame.
  absl::StatusOr<std::shared_ptr<const gpu::Image>> Copy(const CameraDecodedFrame& frame, const CameraRegion& region);
  // The caller must retain each returned image through compositor completion.
  // Destroy the pool after stopping its worker; outstanding images stay valid.
 private:
  struct Impl;
  explicit CameraImages(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};
} // namespace hm::scoreboard
