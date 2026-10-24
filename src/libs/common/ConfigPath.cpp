#include "hstream/src/libs/common/ConfigPath.h"

#include <algorithm>
#include <limits>
#include <vector>

#include "absl/strings/str_split.h"

namespace hm {
bool has_node(const YAML::Node& n, const std::string& dot_string, bool non_null) {
  if (dot_string.empty()) {
    return true;
  }
  const auto node = get_node(n, dot_string);
  return node.has_value() && (!non_null || !node->IsNull());
}

/**
 * Split a dot-delimited string into its component parts
 *
 * @param input The dot-delimited string to split
 * @return A vector of string components
 */
std::vector<std::string> split_by_dot(const std::string& input) {
  std::vector<std::string> result;
  std::string current;

  for (char c : input) {
    if (c == '.') {
      if (!current.empty()) {
        result.push_back(current);
        current.clear();
      }
    } else {
      current += c;
    }
  }

  if (!current.empty()) {
    result.push_back(current);
  }

  return result;
}

/**
 * Set a value in a YAML node based on a dot-delimited path
 *
 * @param node The YAML node to modify
 * @param dot_string A dot-delimited string representing the path to the value
 * @param value The string value to set
 */
YAML::Node set_node_value(YAML::Node node, const std::string& dot_string, const std::string& value) {
  std::vector<std::string> path = split_by_dot(dot_string);

  if (path.empty()) {
    return node; // Nothing to do with an empty path
  }

  std::vector<YAML::Node> current_node;
  current_node.push_back(node);

  // Navigate to the second-to-last element in the path, creating nodes as needed
  for (size_t i = 0; i < path.size() - 1; ++i) {
    const std::string& key = path[i];
    YAML::Node& current = current_node.back();
    // Check if the key exists and is a map
    if (!current[key] || !current[key].IsMap()) {
      // Create a new map node if it doesn't exist or isn't a map
      current[key] = YAML::Node(YAML::NodeType::Map);
    }
    current_node.push_back(current[key]);
  }

  // Set the value at the final path element
  current_node.back()[path.back()] = value;
  return node;
}
std::optional<YAML::Node> get_node(const YAML::Node& n, const std::string& dot_string) {
  if (dot_string.empty()) {
    return n;
  }
  std::vector<std::string> parts = absl::StrSplit(dot_string, '.');

  // Keep a live handle throughout traversal. reset() rebinds the handle;
  // YAML::Node assignment can instead overwrite the aliased source document.
  YAML::Node current = n;
  for (const auto& part : parts) {
    std::string key = part;
    std::optional<std::size_t> idx;
    auto colon_pos = part.find(':');
    if (colon_pos != std::string::npos) {
      key = part.substr(0, colon_pos);
      std::string idx_str = part.substr(colon_pos + 1);
      if (idx_str.empty() ||
          !std::all_of(idx_str.begin(), idx_str.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        return std::nullopt;
      try {
        const auto value = std::stoull(idx_str);
        if (value > std::numeric_limits<std::size_t>::max())
          return std::nullopt;
        idx = static_cast<std::size_t>(value);
      } catch (const std::exception&) {
        // invalid integer
        return std::nullopt;
      }
    }

    if (!key.empty() || !idx.has_value()) {
      if (!current.IsMap()) {
        return std::nullopt;
      }
      const YAML::Node child = static_cast<const YAML::Node&>(current)[key];
      if (!child.IsDefined()) {
        return std::nullopt;
      }
      current.reset(child);
    }

    if (idx.has_value()) {
      if (!current.IsSequence() || *idx >= current.size()) {
        return std::nullopt;
      }
      const YAML::Node child = static_cast<const YAML::Node&>(current)[*idx];
      if (!child.IsDefined()) {
        return std::nullopt;
      }
      current.reset(child);
    }
  }

  return current;
}

} // namespace hm
