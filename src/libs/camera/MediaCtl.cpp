#include "hstream/src/libs/camera/MediaCtl.h"

#include <cstdio>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace hm {
namespace camera {

std::vector<MediaEntity> parseMediaCtlOutput(const std::string& input) {
  std::vector<MediaEntity> entities;
  std::istringstream iss(input);
  std::string line;

  MediaEntity currentEntity;
  EntityPad currentPad;

  std::regex entityRegex(R"(- entity (\d+): (.+?) \((.+?)\))");
  std::regex padRegex(R"(pad(\d+): (Sink|Source))");
  std::regex linkRegex(R"((<-|->) \"(.+?)\":\d+ \[(ENABLED|DISABLED)\])");
  std::regex deviceNodeRegex(R"(device node name (.+))");

  while (std::getline(iss, line)) {
    std::smatch match;

    if (std::regex_search(line, match, entityRegex)) {
      if (currentEntity.id != 0) {
        if (!currentPad.links.empty())
          currentEntity.pads.push_back(currentPad);
        entities.push_back(currentEntity);
        currentEntity = {};
        currentPad = {};
      }

      currentEntity.id = std::stoi(match[1]);
      currentEntity.name = match[2];
    } else if (line.find("type") != std::string::npos && line.find("subtype") != std::string::npos) {
      std::size_t typePos = line.find("type") + 5;
      std::size_t subtypePos = line.find("subtype") + 8;
      currentEntity.type = line.substr(typePos, line.find("subtype") - typePos - 1);
      currentEntity.subtype = line.substr(subtypePos, line.find("flags") - subtypePos - 1);
    } else if (std::regex_search(line, match, deviceNodeRegex)) {
      currentEntity.deviceNode = match[1];
    } else if (std::regex_search(line, match, padRegex)) {
      if (!currentPad.links.empty())
        currentEntity.pads.push_back(currentPad);
      currentPad = {};
      currentPad.padNumber = std::stoi(match[1]);
      currentPad.type = match[2];
    } else if (std::regex_search(line, match, linkRegex)) {
      PadLink link;
      link.direction = match[1];
      link.target = match[2];
      link.enabled = (match[3] == "ENABLED");
      currentPad.links.push_back(link);
    }
  }

  // Add last entity
  if (currentEntity.id != 0) {
    if (!currentPad.links.empty())
      currentEntity.pads.push_back(currentPad);
    entities.push_back(currentEntity);
  }

  return entities;
}

} // namespace camera
} // namespace hm
