#pragma once

#include <gst/gst.h>
#include <gstreamer-1.0/gst/gstobject.h>

#include <iostream>
#include <optional>
#include <string>
#include <vector>

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

YAML::Node set_node_value(YAML::Node node, const std::string& dot_string, const std::string& value);

template <typename T>
inline std::vector<T*> glist_to_vect(const GList* list, size_t reserve_count = 512) {
  std::vector<T*> results;
  results.reserve(reserve_count);
  while (list) {
    results.emplace_back((T*)list->data);
    list = list->next;
  }
  return results;
}

struct Videoinfo {
  int width{0};
  int height{0};
  double fps{0.0};
  size_t frame_count{0};
  size_t video_bit_rate{0};
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

bool connectElementsWithGhostPads(
    GstElement* elem1,
    const char* pad1_name,
    GstElement* elem2,
    const char* pad2_name,
    const std::string& ghost_pad_name);

template <typename G_OBJ>
class GstReferencedObject {
 public:
  GstReferencedObject(G_OBJ obj, bool unref = true) : obj_(obj), unref_(unref) {}

  virtual ~GstReferencedObject() {
    if (obj_ && unref_) {
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
  bool unref_;
};

GstReferencedObject<GstElement*> get_pipeline_element(GstElement* element);

GstMessage* gst_nvmessage_force_pipeline_eos(GstObject* obj, bool force_eos);

bool post_force_pipeline_eos(GstElement* element);

bool gst_message_parse_force_pipeline_eos(GstMessage* message, bool* force_eos);

bool gst_message_is_force_pipeline_eos(GstMessage* message);

/* On GStreamer < 1.20, define request_pad_simple in terms of the
 * old gst_element_get_request_pad().
 *
 * gst_element_get_request_pad():
 *   GstPad* gst_element_get_request_pad(GstElement *element,
 *                                       const gchar *name);
 *   — retrieves a request pad by name; release with gst_element_release_request_pad() :contentReference[oaicite:0]{index=0}
 *
 * gst_element_request_pad_simple():
 *   GstPad* gst_element_request_pad_simple(GstElement *element,
 *                                          const gchar *name);
 *   — introduced in 1.20 as a more explicit name for the same functionality :contentReference[oaicite:1]{index=1}
 */
#if !GST_CHECK_VERSION(1,20,0)
/* Inline function shim */
static inline GstPad *
gst_element_request_pad_simple(GstElement *element, const gchar *name)
{
    return gst_element_get_request_pad(element, name);
}

#endif
} // namespace hm
