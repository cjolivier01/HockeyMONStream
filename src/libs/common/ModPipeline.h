#include <gst/gst.h>

#include <thread>

namespace hm {
std::unique_ptr<std::thread> edit_pipeline(GstObject* pipeline);
}
