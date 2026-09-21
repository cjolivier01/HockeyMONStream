#include "hstream/src/libs/common/pipeline_utils.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

bool check_lookups() {
  const YAML::Node root = YAML::Load(R"yaml(
answer: 42
stitching:
  calibration_frame_count: 4
  calibration_frame_selection:
    context:
      decode_anchor_ns: 582000000000
unset: null
cameras:
  - offset: 1
  - offset: 2
matrix: [[3, 4], [5, 6]]
)yaml");
  const YAML::Node alias = root;
  const std::string original = YAML::Dump(root);
  bool ok = true;
  for (const auto& [path, value] : std::vector<std::pair<std::string, int>>{
           {"answer", 42},
           {"stitching.calibration_frame_count", 4},
           {"cameras:0.offset", 1},
           {"cameras:1.offset", 2},
           {"cameras.:1.offset", 2},
           {"matrix:1.:0", 5}}) {
    const auto node = hm::get_node(root, path);
    ok &= expect(node.has_value() && node->as<int>() == value, "Wrong lookup result for " + path);
    ok &= expect(hm::has_node(root, path, true), "Existing path not found: " + path);
  }
  ok &= expect(
      hm::get_node_value<uint64_t>(root, "stitching.calibration_frame_selection.context.decode_anchor_ns", 0) ==
          582000000000ULL,
      "Deep calibration settings must retain their exact values");
  const auto sequence_item = hm::get_node(root["cameras"], ":1.offset");
  ok &= expect(sequence_item.has_value() && sequence_item->as<int>() == 2, "Root sequence index lookup failed");
  const auto entire_document = hm::get_node(root, "");
  ok &= expect(
      entire_document.has_value() && YAML::Dump(*entire_document) == original && hm::has_node(root, "", true),
      "An empty path must retain the root document");
  const auto null_node = hm::get_node(root, "unset");
  ok &= expect(
      null_node.has_value() && null_node->IsNull() && hm::has_node(root, "unset", false) &&
          !hm::has_node(root, "unset", true),
      "Null entries must remain distinguishable from missing entries");

  for (const char* path :
       {"missing",
        "stitching.missing.deep",
        "answer.child",
        "unset.child",
        "cameras.offset",
        "cameras:2.offset",
        "cameras:-1",
        "cameras:+1",
        "cameras:1junk",
        "cameras:",
        "cameras:999999999999999999999999999999",
        "answer:0",
        ":0"}) {
    ok &= expect(!hm::get_node(root, path).has_value(), "Invalid path should not resolve: " + std::string(path));
    ok &= expect(!hm::has_node(root, path, false), "Invalid path should not exist: " + std::string(path));
  }
  ok &= expect(
      hm::get_node_value<int>(root, "stitching.missing", 17) == 17, "Missing settings must use the supplied default");
  ok &= expect(
      YAML::Dump(root) == original && YAML::Dump(alias) == original,
      "Successful and missing lookups must leave the original document and its aliases unchanged");

  std::optional<YAML::Node> retained;
  {
    const YAML::Node temporary = YAML::Load("outer: {inner: 7}");
    retained = hm::get_node(temporary, "outer.inner");
  }
  ok &= expect(retained.has_value() && retained->as<int>() == 7, "Returned nodes must retain their backing document");
  return ok;
}

} // namespace

int main() {
  try {
    return check_lookups() ? 0 : 1;
  } catch (const std::exception& exception) {
    std::cerr << "YAML path lookup unexpectedly threw: " << exception.what() << '\n';
    return 1;
  }
}
