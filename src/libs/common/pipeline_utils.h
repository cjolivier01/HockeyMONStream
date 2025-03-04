#include <gst/gst.h>

#include "absl/strings/str_split.h"
#include "yaml-cpp/yaml.h"

#include <optional>
#include <sstream>
#include <string>

namespace hm {

bool has_node(const YAML::Node& n, const std::string& dot_string, bool non_null);

std::optional<YAML::Node> get_node(const YAML::Node&, const std::string& dot_string);

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
  size_t audio_samples_per_second{0};
  size_t num_audio_channels{0};
  size_t num_audio_streams{0};
};

Videoinfo getVideoInfo(const std::string& videoPath);

} // namespace hm

#define TO_STRING(_stuff$) (std::stringstream() << _stuff$).str()
