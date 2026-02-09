#pragma once

#include <string>
#include <vector>

namespace hm {
namespace camera {

struct PadLink {
  std::string direction; // "<-" or "->"
  std::string target;
  bool enabled;
};

struct EntityPad {
  int padNumber;
  std::string type; // Sink or Source
  std::vector<PadLink> links;
};

struct MediaEntity {
  int id;
  std::string name;
  std::string type;
  std::string subtype;
  std::string deviceNode;
  std::vector<EntityPad> pads;
};

std::vector<MediaEntity> parseMediaCtlOutput(const std::string& input);

} // namespace camera
} // namespace hm
