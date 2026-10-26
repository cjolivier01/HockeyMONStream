#pragma once
#include <string_view>

namespace hm {
// Semantic detector labels, never arbitrary nonzero COCO class indices.
inline bool IsGoalie(std::string_view label) {
  return label == "goalie" || label == "keeper";
}
inline bool IsReferee(std::string_view label) {
  return label == "official" || label == "referee" || label == "ref";
}
inline bool IsPerson(int class_id, std::string_view label) {
  return class_id == 0 || IsGoalie(label) || IsReferee(label);
}
inline const char* PersonRoleText(std::string_view label) {
  return IsGoalie(label) ? "GOALIE" : IsReferee(label) ? "REF" : "";
}
} // namespace hm
