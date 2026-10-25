#include "hstream/src/libs/program/ToneControlCommand.h"

#include <charconv>
#include <limits>
#include <set>
#include <sstream>

#include "hstream/src/libs/program/ToneControls.h"

namespace hm::program {
namespace {
template <typename T>
bool Integer(const std::string& text, T* value) {
  if (text.empty())
    return false;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
} // namespace

absl::Status ParseToneControlCommand(std::string_view line, ToneControlCommand* command) {
  if (!command)
    return absl::InvalidArgumentError("Missing tone command destination");
  *command = {};
  // Protocol allocation bound, not a runtime/configuration default.
  if (line.size() > 1024 || line.find('\0') != std::string_view::npos)
    return absl::InvalidArgumentError("Tone command exceeds its protocol bounds");
  std::istringstream input{std::string(line)};
  std::string verb, request, stage, generation;
  input >> verb >> request >> stage >> generation;
  if (verb != "set-tone-controls" || !Integer(request, &command->request_id) || !command->request_id ||
      command->request_id > uint64_t(std::numeric_limits<int64_t>::max())) {
    command->request_id = 0;
    return absl::InvalidArgumentError("Tone command requires a positive signed-64-bit request ID");
  }
  if (!Integer(stage, &command->stage) || !Integer(generation, &command->generation) || !command->generation ||
      command->generation > uint64_t(std::numeric_limits<int64_t>::max())) {
    command->generation = 0;
    return absl::InvalidArgumentError("Tone command requires a stage and positive topology generation");
  }
  std::set<std::string> seen;
  std::string assignment;
  while (input >> assignment) {
    if (command->controls.size() == kToneControls.size())
      return absl::InvalidArgumentError("Tone command contains too many controls");
    const auto equals = assignment.find('=');
    const std::string id = assignment.substr(0, equals);
    int value = 0;
    if (equals == std::string::npos || !Integer(assignment.substr(equals + 1), &value))
      return absl::InvalidArgumentError("Tone controls require ID=integer presentation values");
    if (!seen.insert(id).second)
      return absl::InvalidArgumentError("Tone command repeats a control ID");
    const auto property = BuildTonePropertyCommand(id, value, false);
    if (!property.ok())
      return property.status();
    command->controls.push_back({id, value});
  }
  if (command->controls.empty())
    return absl::InvalidArgumentError("Tone command requires at least one control");
  return absl::OkStatus();
}
} // namespace hm::program
