#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"

namespace hm::program {
struct ToneControlValue {
  std::string id;
  int value;
};

struct ToneControlCommand {
  uint64_t request_id{0};
  int64_t stage{0};
  uint64_t generation{0};
  std::vector<ToneControlValue> controls;
};

// The stdin reader removes '@'. The bounded command contains only canonical
// control IDs and integral presentation values; no elements, paths or defaults.
// On failure, a valid parsed header remains available for a correlated error.
absl::Status ParseToneControlCommand(std::string_view line, ToneControlCommand* command);
} // namespace hm::program
