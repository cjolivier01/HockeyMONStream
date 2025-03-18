#pragma once

#include <gst/gst.h>
#include <gstreamer-1.0/gst/gstobject.h>

#include <optional>
#include <string>

#include "yaml-cpp/yaml.h"

namespace hm {

bool has_node(const YAML::Node& n, const std::string& dot_string, bool non_null);

std::optional<YAML::Node> get_node(const YAML::Node& n, const std::string& dot_string);

void save_dot_file(GstElement* pipeline, GstDebugGraphDetails details, const std::string& filename);

bool seek_element(GstElement* seek_element, size_t seek_to_nanoseconds);

template <typename T>
inline T get_node_as(const YAML::Node& n, const std::string& dot_string, const T& dflt) {
  std::optional<YAML::Node> o_n = get_node(n, dot_string);
  if (!o_n.has_value()) {
    return dflt;
  }
  return o_n.value().as<T>();
}

struct Videoinfo {
  int width{0};
  int height{0};
  double fps{0.0};
  size_t frame_count{0};
  size_t video_bit_rate{55000000};
  // size_t audio_samples_per_second{0};
  // size_t num_audio_channels{0};
  // size_t num_audio_streams{0};
};

Videoinfo getVideoInfo(const std::string& videoPath);

template <typename T>
inline T get_node_value(const YAML::Node& n, const std::string& dot_string, const T& default_value) {
  std::optional<YAML::Node> o_n = hm::get_node(n, dot_string);
  if (!o_n.has_value()) {
    return default_value;
  }
  if constexpr (std::is_same_v<T, bool>) {
    // We serialize bools as int so that we don`'t have to write true/false all
    // the time
    return !!o_n.value().as<int>();
  }
  return o_n.value().as<T>();
}

const char* gstStateToString(GstState state);

void waitForPipelineStop(GstElement* pipeline);

GstCaps* setCapsDimensions(GstCaps* caps, int width, int height);
bool getCapsDimensions(GstCaps* caps, int& width, int& height);

template <typename G_OBJ>
class GstReferencedObject {
 public:
  GstReferencedObject(G_OBJ obj) : obj_(obj) {}

  virtual ~GstReferencedObject() {
    if (obj_) {
      gst_object_unref(obj_);
    }
  }
  operator G_OBJ() {
    return obj_;
  }
  operator const G_OBJ() const {
    return obj_;
  }
  G_OBJ get() {
    return obj_;
  }
  const G_OBJ get() const {
    return obj_;
  }
  G_OBJ& operator->() {
    assert(obj_);
    return obj_;
  }
  const G_OBJ& operator->() const {
    assert(obj_);
    return obj_;
  }
  operator bool() const {
    return obj_ != nullptr;
  }
  void release() {
    obj_ = nullptr;
  }

 private:
  G_OBJ obj_;
};

GstReferencedObject<GstElement*> get_pipeline_element(GstElement* element);

GstMessage* gst_nvmessage_force_pipeline_eos(GstObject* obj, bool force_eos);

bool post_force_pipeline_eos(GstElement* element);

bool gst_message_parse_force_pipeline_eos(GstMessage* message, bool* force_eos);

bool gst_message_is_force_pipeline_eos(GstMessage* message);

} // namespace hm
