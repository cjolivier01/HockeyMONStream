#pragma once

#include <nvbufsurface.h>

#include <memory>

#include "absl/status/statusor.h"
#include "hstream/src/libs/player_analytics/runtime/PoseGpu.h"

namespace hm::player_analytics {

// Imports NVMM/EGL without staging pixels. The caller must synchronize its
// stream before releasing this import. A direct CUDA surface needs no import.
class BorrowedImage {
 public:
  static absl::StatusOr<std::unique_ptr<BorrowedImage>> Map(NvBufSurface* surface, unsigned index, int gpu_id);
  ~BorrowedImage();
  const ImageView& view() const noexcept {
    return view_;
  }

 private:
  struct Import;
  BorrowedImage();
  ImageView view_;
  std::unique_ptr<Import> import_;
};

} // namespace hm::player_analytics
