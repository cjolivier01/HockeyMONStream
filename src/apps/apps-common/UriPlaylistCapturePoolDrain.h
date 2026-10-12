#pragma once

#include <gst/gst.h>

#include <vector>

namespace hm {

// A stopped decoder's capture pool stays alive while queued/borrowed frames
// remain downstream. Retain one reference so the last unref (including vendor
// allocator/decoder destruction) can complete synchronously before restarting.
// Observe runs on the serialized decoder stream; Drain runs after decoder NULL
// has joined that stream and released the decoder's owning references.
class UriPlaylistCapturePoolDrain {
 public:
  UriPlaylistCapturePoolDrain() = default;
  UriPlaylistCapturePoolDrain(const UriPlaylistCapturePoolDrain&) = delete;
  UriPlaylistCapturePoolDrain& operator=(const UriPlaylistCapturePoolDrain&) = delete;
  ~UriPlaylistCapturePoolDrain();
  void Observe(GstBuffer* buffer);
  bool Drain(gint64 finalizer_budget_us = 30 * G_TIME_SPAN_SECOND);
  void Release();

 private:
  std::vector<GstBufferPool*> observed_;
};

} // namespace hm
