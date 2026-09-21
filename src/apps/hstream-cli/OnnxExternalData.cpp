#include "OnnxExternalData.h"

#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hm::pipeline {
namespace {

enum class Message { kModel, kGraph, kNode, kAttribute, kTensor, kSparseTensor, kFunction, kTraining, kEntry };

bool read_varint(std::string_view& input, uint64_t& value) {
  value = 0;
  for (unsigned shift = 0; shift < 64 && !input.empty(); shift += 7) {
    const auto byte = static_cast<unsigned char>(input.front());
    input.remove_prefix(1);
    if (shift == 63 && byte > 1)
      return false;
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0)
      return true;
  }
  return false;
}

// Only traverse message fields from the ONNX schema. In particular, tensor
// raw_data and arbitrary strings must never be interpreted as nested messages.
bool child_message(Message parent, unsigned field, Message& child) {
  switch (parent) {
    case Message::kModel:
      if (field == 7) {
        child = Message::kGraph;
        return true;
      }
      if (field == 20) {
        child = Message::kTraining;
        return true;
      }
      if (field == 25) {
        child = Message::kFunction;
        return true;
      }
      break;
    case Message::kGraph:
      if (field == 1) {
        child = Message::kNode;
        return true;
      }
      if (field == 5) {
        child = Message::kTensor;
        return true;
      }
      if (field == 15) {
        child = Message::kSparseTensor;
        return true;
      }
      break;
    case Message::kNode:
      if (field == 5) {
        child = Message::kAttribute;
        return true;
      }
      break;
    case Message::kAttribute:
      if (field == 5 || field == 10) {
        child = Message::kTensor;
        return true;
      }
      if (field == 6 || field == 11) {
        child = Message::kGraph;
        return true;
      }
      if (field == 22 || field == 23) {
        child = Message::kSparseTensor;
        return true;
      }
      break;
    case Message::kSparseTensor:
      if (field == 1 || field == 2) {
        child = Message::kTensor;
        return true;
      }
      break;
    case Message::kFunction:
      if (field == 7) {
        child = Message::kNode;
        return true;
      }
      if (field == 11) {
        child = Message::kAttribute;
        return true;
      }
      break;
    case Message::kTraining:
      if (field == 1 || field == 2) {
        child = Message::kGraph;
        return true;
      }
      break;
    case Message::kTensor:
      if (field == 13) {
        child = Message::kEntry;
        return true;
      }
      break;
    case Message::kEntry:
      break;
  }
  return false;
}

bool inspect_message(
    std::string_view input,
    Message kind,
    unsigned depth,
    std::set<std::filesystem::path>& locations,
    std::string& entry_location) {
  if (depth > 64)
    return false;
  bool external = false;
  std::string location, key, value;
  while (!input.empty()) {
    uint64_t tag = 0, scalar = 0;
    if (!read_varint(input, tag) || tag < 8 || (tag >> 3) > 0x1fffffff)
      return false;
    const unsigned field = tag >> 3;
    const unsigned wire = tag & 7;
    std::string_view bytes;
    if (wire == 0) {
      if (!read_varint(input, scalar))
        return false;
    } else {
      uint64_t length = 0;
      if (wire == 2) {
        if (!read_varint(input, length))
          return false;
      } else if (wire == 1 || wire == 5) {
        length = wire == 1 ? 8 : 4;
      } else {
        return false;
      }
      if (length > input.size())
        return false;
      bytes = input.substr(0, length);
      input.remove_prefix(length);
    }
    Message child;
    if (child_message(kind, field, child)) {
      if (wire != 2 || !inspect_message(bytes, child, depth + 1, locations, location))
        return false;
    } else if (kind == Message::kTensor && field == 14) {
      if (wire != 0)
        return false;
      external = scalar == 1;
    } else if (kind == Message::kEntry && (field == 1 || field == 2)) {
      if (wire != 2)
        return false;
      (field == 1 ? key : value).assign(bytes);
    }
  }
  if (kind == Message::kEntry && key == "location")
    entry_location = value;
  if (kind == Message::kTensor && external) {
    const std::filesystem::path path(location);
    if (location.empty() || location.find('\0') != std::string::npos || path.is_absolute() || path.filename().empty())
      return false;
    for (const auto& component : path)
      if (component == "..")
        return false;
    locations.insert(path.lexically_normal());
  }
  return true;
}

} // namespace

absl::StatusOr<std::vector<std::filesystem::path>> OnnxExternalDataFiles(const std::filesystem::path& model) {
  const int fd = ::open(model.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return absl::NotFoundError("Unable to open ONNX model: " + model.string());
  struct stat status{};
  if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0 ||
      static_cast<uint64_t>(status.st_size) > std::numeric_limits<size_t>::max()) {
    ::close(fd);
    return absl::InvalidArgumentError("ONNX model must be a nonempty regular file: " + model.string());
  }
  const size_t size = status.st_size;
  void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (mapped == MAP_FAILED)
    return absl::InternalError("Unable to map ONNX metadata: " + model.string());
  std::set<std::filesystem::path> locations;
  std::string unused;
  const bool valid =
      inspect_message(std::string_view(static_cast<const char*>(mapped), size), Message::kModel, 0, locations, unused);
  ::munmap(mapped, size);
  if (!valid)
    return absl::InvalidArgumentError("Invalid ONNX metadata or unsafe external tensor location: " + model.string());
  return std::vector<std::filesystem::path>(locations.begin(), locations.end());
}

} // namespace hm::pipeline
