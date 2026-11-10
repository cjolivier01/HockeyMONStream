#include "hstream/src/libs/onnx/ImageInput.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace hm::onnx {
namespace {
void Require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct File {
  int fd;
  ~File() {
    if (fd >= 0)
      close(fd);
  }
};
struct Field {
  unsigned number, wire;
  uint64_t value, begin, end;
};
// Each cursor is confined to one protobuf message. Skip payloads by offset;
// no model-sized mapping/allocation or inference SDK is needed for discovery.
struct Cursor {
  int fd;
  uint64_t position, end;
  uint64_t Varint() {
    uint64_t result = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      Require(position < end, "Truncated ONNX varint");
      unsigned char byte;
      ssize_t count;
      do {
        count = pread(fd, &byte, 1, position);
      } while (count < 0 && errno == EINTR);
      Require(count == 1, "Cannot read ONNX metadata");
      ++position;
      Require(shift != 63 || byte <= 1, "Overflowed ONNX varint");
      result |= uint64_t(byte & 0x7f) << shift;
      if (!(byte & 0x80))
        return result;
    }
    throw std::runtime_error("Unterminated ONNX varint");
  }
  bool Next(Field& field) {
    if (position == end)
      return false;
    const auto tag = Varint();
    Require(tag >= 8 && (tag >> 3) <= 0x1fffffff, "Invalid ONNX field number");
    field = {unsigned(tag >> 3), unsigned(tag & 7), 0, 0, 0};
    if (field.wire == 0) {
      field.value = Varint();
      return true;
    }
    Require(field.wire == 1 || field.wire == 2 || field.wire == 5, "Unsupported ONNX wire type");
    const uint64_t length = field.wire == 2 ? Varint() : field.wire == 1 ? 8 : 4;
    Require(length <= end - position, "ONNX field exceeds its containing message");
    field.begin = position;
    field.end = position += length;
    return true;
  }
  Cursor Message(const Field& field) const {
    Require(field.wire == 2, "Expected an ONNX message");
    return {fd, field.begin, field.end};
  }
  std::string String(const Field& field) const {
    Require(
        field.wire == 2 && field.end > field.begin && field.end - field.begin <= 4096,
        "ONNX input/dimension name must contain 1 through 4096 bytes");
    std::string result(field.end - field.begin, '\0');
    size_t done = 0;
    while (done < result.size()) {
      const auto count = pread(fd, result.data() + done, result.size() - done, field.begin + done);
      if (count < 0 && errno == EINTR)
        continue;
      Require(count > 0, "Cannot read ONNX input/dimension name");
      done += count;
    }
    Require(result.find('\0') == std::string::npos, "ONNX name contains a NUL byte");
    return result;
  }
};
int Dimension(Cursor cursor) {
  Field field;
  bool found = false;
  int value = 0;
  while (cursor.Next(field)) {
    if (field.number != 1 && field.number != 2)
      continue;
    Require(!found, "Ambiguous ONNX dimension");
    found = true;
    if (field.number == 1) {
      Require(
          field.wire == 0 && field.value > 0 && field.value <= uint64_t(INT_MAX),
          "ONNX dimension must be a positive integer");
      value = field.value;
    } else {
      cursor.String(field);
      value = -1;
    }
  }
  Require(found, "ONNX dimension must be static or named");
  return value;
}
std::vector<int> Shape(Cursor cursor) {
  Field field;
  std::vector<int> dimensions;
  while (cursor.Next(field)) {
    if (field.number == 1) {
      Require(dimensions.size() < 4, "ONNX detector input must have four dimensions");
      dimensions.push_back(Dimension(cursor.Message(field)));
    }
  }
  Require(
      dimensions.size() == 4 && (dimensions[0] == 1 || dimensions[0] == -1) && dimensions[1] == 3 &&
          dimensions[2] > 0 && dimensions[3] > 0,
      "ONNX detector requires [1 or symbolic batch, 3, static height, static width]");
  return dimensions;
}
std::vector<int> TensorType(Cursor cursor) {
  Field field;
  bool type = false, shape = false;
  std::vector<int> dimensions;
  while (cursor.Next(field)) {
    if (field.number == 1) {
      Require(!type && field.wire == 0 && field.value == 1, "ONNX detector input must be float32");
      type = true;
    } else if (field.number == 2) {
      Require(!shape, "Duplicate ONNX input shape");
      shape = true;
      dimensions = Shape(cursor.Message(field));
    }
  }
  Require(type && shape, "Missing ONNX input type or shape");
  return dimensions;
}
std::vector<int> Type(Cursor cursor) {
  Field field;
  bool found = false;
  std::vector<int> dimensions;
  while (cursor.Next(field)) {
    if (field.number == 1) {
      Require(!found, "Duplicate ONNX tensor type");
      found = true;
      dimensions = TensorType(cursor.Message(field));
    } else if (field.number == 4 || field.number == 5 || field.number == 8 || field.number == 9) {
      throw std::runtime_error("ONNX detector input must be a dense tensor");
    }
  }
  Require(found, "Missing ONNX tensor type");
  return dimensions;
}
ImageInput Input(Cursor cursor) {
  Field field;
  ImageInput input{};
  bool type = false;
  while (cursor.Next(field)) {
    if (field.number == 1) {
      Require(input.name.empty(), "Duplicate ONNX input name");
      input.name = cursor.String(field);
    } else if (field.number == 2) {
      Require(!type, "Duplicate ONNX input type");
      type = true;
      const auto shape = Type(cursor.Message(field));
      input.width = shape[3];
      input.height = shape[2];
    }
  }
  Require(!input.name.empty() && type, "Missing ONNX input name or type");
  return input;
}
ImageInput Graph(Cursor cursor) {
  Field field;
  bool found = false;
  ImageInput input{};
  while (cursor.Next(field)) {
    if (field.number == 11) {
      Require(!found, "ONNX detector must have exactly one graph input");
      found = true;
      input = Input(cursor.Message(field));
    }
  }
  Require(found, "ONNX graph has no input");
  return input;
}
} // namespace
absl::StatusOr<ImageInput> ReadImageInput(const std::filesystem::path& model) {
  File file{open(model.c_str(), O_RDONLY | O_CLOEXEC)};
  if (file.fd < 0)
    return absl::NotFoundError("Cannot open ONNX model: " + model.string());
  struct stat info {};
  if (fstat(file.fd, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0)
    return absl::InvalidArgumentError("ONNX model must be a nonempty regular file");
  try {
    Cursor cursor{file.fd, 0, uint64_t(info.st_size)};
    Field field;
    bool found = false;
    ImageInput input{};
    while (cursor.Next(field)) {
      if (field.number == 7) {
        Require(!found, "Duplicate ONNX graph");
        found = true;
        input = Graph(cursor.Message(field));
      }
    }
    Require(found, "ONNX model has no graph");
    return input;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("ONNX image input: " + std::string(error.what()));
  }
}
} // namespace hm::onnx
