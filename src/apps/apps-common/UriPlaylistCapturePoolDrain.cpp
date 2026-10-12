#include "hstream/src/apps/apps-common/UriPlaylistCapturePoolDrain.h"

#include "hstream/src/libs/common/ShutdownWatchdog.h"

#include <algorithm>

namespace hm {

UriPlaylistCapturePoolDrain::~UriPlaylistCapturePoolDrain() {
  Release();
}

void UriPlaylistCapturePoolDrain::Release() {
  for (GstBufferPool* pool : observed_) {
    gst_object_unref(pool);
  }
  observed_.clear();
}

void UriPlaylistCapturePoolDrain::Observe(GstBuffer* buffer) {
  // Read this before making the decoder buffer writable: a shallow copy has
  // no pool of its own even though it still borrows the original's memory.
  GstBufferPool* pool = buffer ? buffer->pool : nullptr;
  if (!pool || std::find(observed_.begin(), observed_.end(), pool) != observed_.end()) {
    return;
  }
  observed_.push_back(GST_BUFFER_POOL(gst_object_ref(pool)));
}

bool UriPlaylistCapturePoolDrain::Drain(gint64 finalizer_budget_us) {
  for (auto it = observed_.begin(); it != observed_.end();) {
    // There is no producer after decoder NULL. Each outstanding buffer owns a
    // pool reference until release_buffer and do_stop have returned. Once ours
    // is the only reference, nobody can acquire another through a live owner.
    // A weak notification is too early: it precedes the vendor finalizer.
    if (GST_OBJECT_REFCOUNT_VALUE(*it) == 1) {
      ShutdownWatchdog watchdog("Camera chapter capture-pool destruction", finalizer_budget_us);
      gst_object_unref(*it);
      it = observed_.erase(it);
    } else {
      ++it;
    }
  }
  return observed_.empty();
}

} // namespace hm
