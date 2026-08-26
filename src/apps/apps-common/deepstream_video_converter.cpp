#include "hstream/src/apps/apps-common/deepstream_config.h"

#include <gst/gst.h>

#include <atomic>
#include <cstring>

namespace {

const char* normalize_video_converter_element_name(const char* element_name) {
  if (element_name == nullptr || *element_name == '\0') {
    return nullptr;
  }
  if (std::strcmp(element_name, hm::deepstream::kNvVideoConvertElement) == 0) {
    return hm::deepstream::kNvVideoConvertElement;
  }
  if (std::strcmp(element_name, hm::deepstream::kDsxVideoConvertElement) == 0) {
    return hm::deepstream::kDsxVideoConvertElement;
  }
  return nullptr;
}

std::atomic<const char*> g_video_converter_element{hm::deepstream::kNvVideoConvertElement};

} // namespace

namespace hm::deepstream {

const char* default_video_converter_element_name() {
  return kNvVideoConvertElement;
}

const char* video_converter_element_name() {
  const char* element_name = g_video_converter_element.load(std::memory_order_acquire);
  return element_name == nullptr ? default_video_converter_element_name() : element_name;
}

bool is_supported_video_converter_element_name(const char* element_name) {
  return normalize_video_converter_element_name(element_name) != nullptr;
}

bool video_converter_element_factory_available(const char* element_name) {
  const char* normalized = normalize_video_converter_element_name(element_name);
  if (normalized == nullptr) {
    return false;
  }
  GstElementFactory* factory = gst_element_factory_find(normalized);
  if (factory == nullptr) {
    return false;
  }
  gst_object_unref(factory);
  return true;
}

bool set_video_converter_element_name(const char* element_name) {
  const char* normalized = normalize_video_converter_element_name(element_name);
  if (normalized == nullptr) {
    return false;
  }
  g_video_converter_element.store(normalized, std::memory_order_release);
  return true;
}

} // namespace hm::deepstream
