#include <gst/gst.h>

#include "absl/strings/str_split.h"
#include "yaml-cpp/yaml.h"

#include <optional>
#include <string>

namespace hm {

bool has_node(YAML::Node n, const std::string& dot_string);

std::optional<YAML::Node> get_node(YAML::Node n, const std::string& dot_string);

void save_dot_file(GstElement* pipeline, GstDebugGraphDetails details, const std::string& filename);

bool seek_element(GstElement* seek_element, size_t seek_to_nanoseconds);

} // namespace hm
