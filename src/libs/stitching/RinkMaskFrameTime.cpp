#include "hstream/src/libs/stitching/RinkMaskFrameTime.h"

#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>

namespace hm::stitching {

absl::StatusOr<std::optional<int64_t>> ParseRinkMaskFrameTime(const std::string& value) {
  if (value == "auto")
    return std::nullopt;
  static const std::regex syntax(R"(^(-?)([0-9]{2,6}):([0-5][0-9]):([0-5][0-9])(?:[.]([0-9]{1,3}))?$)");
  std::smatch match;
  if (!std::regex_match(value, match, syntax))
    return absl::InvalidArgumentError("Rink mask frame time must be auto or [-]HH:MM:SS[.mmm]");
  const int64_t seconds = std::stoll(match[2]) * 3600 + std::stoll(match[3]) * 60 + std::stoll(match[4]);
  std::string fraction = match[5];
  fraction.append(3 - fraction.size(), '0');
  const int64_t milliseconds = std::stoll(fraction);
  constexpr int64_t kSecond = 1000000000;
  if (seconds > (std::numeric_limits<int64_t>::max() - milliseconds * 1000000) / kSecond)
    return absl::InvalidArgumentError("Rink mask frame time is too large");
  const int64_t offset = seconds * kSecond + milliseconds * 1000000;
  const bool negative = match[1] == "-";
  if (negative && offset == 0)
    return absl::InvalidArgumentError("Negative zero selects the recording end, where no frame exists");
  return std::optional<int64_t>(negative ? -offset : offset);
}

std::string FormatRinkMaskFrameTime(std::optional<int64_t> offset_ns) {
  if (!offset_ns)
    return "auto";
  const uint64_t magnitude = *offset_ns < 0 ? uint64_t(-(*offset_ns + 1)) + 1 : uint64_t(*offset_ns);
  const uint64_t seconds = magnitude / 1000000000;
  std::ostringstream result;
  if (*offset_ns < 0)
    result << '-';
  result << std::setfill('0') << std::setw(2) << seconds / 3600 << ':' << std::setw(2) << seconds / 60 % 60 << ':'
         << std::setw(2) << seconds % 60;
  const uint64_t milliseconds = magnitude / 1000000 % 1000;
  if (milliseconds)
    result << '.' << std::setw(3) << milliseconds;
  return result.str();
}

absl::StatusOr<uint64_t> ResolveRinkMaskFrameTime(int64_t offset_ns, uint64_t recording_duration_ns) {
  if (recording_duration_ns == 0 || recording_duration_ns == std::numeric_limits<uint64_t>::max())
    return absl::FailedPreconditionError("Rink mask frame time requires a known, nonempty synchronized recording");
  const uint64_t magnitude = offset_ns < 0 ? uint64_t(-(offset_ns + 1)) + 1 : uint64_t(offset_ns);
  if ((offset_ns < 0 && magnitude > recording_duration_ns) || (offset_ns >= 0 && magnitude >= recording_duration_ns))
    return absl::OutOfRangeError("Rink mask frame time is outside the synchronized recording");
  return offset_ns < 0 ? recording_duration_ns - magnitude : magnitude;
}

} // namespace hm::stitching
