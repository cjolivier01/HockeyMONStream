#pragma once

#include <gst/gst.h>
#include <filesystem>

namespace hm::gpu {
// Transfer-full element, or nullptr if the installed VA plugin has no matching
// factory on this canonical render node. A display context cannot move a VA
// element to another GPU; always select the factory before setting its context.
GstElement* MakeVaElement(const char* suffix, const std::filesystem::path& canonical_render_node);
} // namespace hm::gpu
