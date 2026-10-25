#pragma once
#include "hstream/src/apps/apps-common/WebPreviewObserver.h"

namespace hm::webrtc {
// The caller owns the thread-default GLib context. Start and Stop run on that
// context; stop each server before its graph reaches EOS or changes to NULL.
// A null graph stops all servers (legacy whole-process CLI shutdown).
bool Start(GstElement* endpoint, unsigned port, std::shared_ptr<preview::WebPreviewSignaling> preview);
void Stop(GstElement* graph);
} // namespace hm::webrtc
