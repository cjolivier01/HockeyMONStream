#include "hstream/src/libs/onnx/ImageInput.h"

#include <unistd.h>

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
std::string Varint(uint64_t value) {
  std::string output;
  do {
    const unsigned char byte = value & 127;
    value >>= 7;
    output += char(byte | (value ? 128 : 0));
  } while (value);
  return output;
}
std::string Scalar(unsigned field, uint64_t value) {
  return Varint(field << 3) + Varint(value);
}
std::string Bytes(unsigned field, const std::string& value) {
  return Varint((field << 3) | 2) + Varint(value.size()) + value;
}
std::string Shape(const std::string& batch, const std::string& height, unsigned type = 1) {
  return Scalar(1, type) +
      Bytes(2, Bytes(1, batch) + Bytes(1, Scalar(1, 3)) + Bytes(1, height) + Bytes(1, Scalar(1, 1984)));
}
std::string Input(const std::string& shape, const std::string& name = "images") {
  return Bytes(1, name) + Bytes(2, Bytes(1, shape));
}
std::string Model(const std::string& input) {
  // Binary tensor payload resembles malformed metadata and must be skipped.
  return Scalar(1, 8) + Bytes(7, Bytes(5, std::string("\xff\xff\0", 3)) + Bytes(11, input));
}
} // namespace
int main(int argc, char** argv) {
  std::filesystem::path file;
  try {
    if (argc == 2) {
      const auto input = hm::onnx::ReadImageInput(argv[1]);
      Check(input.ok(), input.status().ToString().c_str());
      std::cout << input->name << ' ' << input->width << 'x' << input->height << '\n';
      return 0;
    }
    std::string temporary = (std::filesystem::temp_directory_path() / "hstream-image-input-XXXXXX").string();
    const int fd = mkstemp(temporary.data());
    Check(fd >= 0, "Cannot create fixture");
    close(fd);
    file = temporary;
    const auto read = [&](const std::string& bytes) {
      {
        std::ofstream output(file, std::ios::binary);
        output.write(bytes.data(), bytes.size());
      }
      return hm::onnx::ReadImageInput(file);
    };
    const auto input = Input(Shape(Bytes(2, "batch"), Scalar(1, 736)));
    const auto model = Model(input);
    auto info = read(model);
    Check(
        info.ok() && info->name == "images" && info->width == 1984 && info->height == 736,
        "Named batch/static RGB input not discovered");
    Check(read(Model(Input(Shape(Scalar(1, 1), Scalar(1, 736))))).ok(), "Static batch one rejected");
    for (const auto& invalid :
         {Model(Input(Shape(Scalar(1, 2), Scalar(1, 736)))),
          Model(Input(Shape(Bytes(2, "batch"), Bytes(2, "height")))),
          Model(Input(Shape(Bytes(2, "batch"), Scalar(1, 0)))),
          Model(Input(Shape(Bytes(2, "batch"), Scalar(1, uint64_t(-1))))),
          Model(Input(Shape(Bytes(2, "batch"), Scalar(1, 736), 10))),
          Model(Input(Shape(Bytes(2, "batch"), Scalar(1, 736)), "")),
          Model(input + Bytes(1, "duplicate")),
          Bytes(7, Bytes(11, input) + Bytes(11, input)),
          Bytes(7, Bytes(12, input)),
          model + Bytes(7, Bytes(11, input)),
          std::string("\x3a\xff\xff\xff\xff\xff\xff\xff\xff\xff\x7f", 11),
          std::string("\x3a\x02\x58", 3),
          std::string("\x3b", 1)}) {
      Check(!read(invalid).ok(), "Unsupported or malformed model accepted");
    }
    for (size_t size = 0; size < model.size(); ++size)
      Check(!read(model.substr(0, size)).ok(), "Truncated ONNX model accepted");
    std::filesystem::remove(file);
    return 0;
  } catch (const std::exception& error) {
    if (!file.empty())
      std::filesystem::remove(file);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
