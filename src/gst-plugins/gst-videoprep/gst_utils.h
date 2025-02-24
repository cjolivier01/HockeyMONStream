#include <gst/gst.h>

namespace hm {
namespace gst {

void inspect_nvbufsurface_dtype(GstBuffer* buffer);

void print_caps(const GstCaps* caps);
void print_caps_details(const GstCaps* caps);
gint get_batch_size_from_caps(GstCaps *caps);

} // namespace gst
} // namespace hm
