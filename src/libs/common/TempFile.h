#pragma once

#include <filesystem>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace hm {
namespace utils {

class TempFile : public ManagedObject {
 public:
  // Constructor: creates a temporary file in the system temp directory.
  // autoRemove controls whether the file gets deleted upon destruction.
  explicit TempFile(bool autoRemove = true) : autoRemove_(autoRemove) {
    // Get the system's temporary directory.
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    // Generate a unique filename using a fallback implementation.
    filePath_ = tempDir / generate_unique_filename();

    // Create the file by opening an output file stream.
    std::ofstream ofs(filePath_);
    if (!ofs) {
      throw std::runtime_error("Failed to create temporary file: " + filePath_.string());
    }
    // The file stream automatically closes when leaving the scope.
  }

  // The destructor removes the file if autoRemove_ is true.
  ~TempFile() override {
    if (autoRemove_) {
      std::error_code ec; // non-throwing removal
      std::filesystem::remove(filePath_, ec);
      if (ec) {
        std::cerr << "Warning: failed to remove temporary file: " << filePath_ << " (" << ec.message() << ")\n";
      }
    }
  }

  // Expose the temporary file's path.
  std::filesystem::path getPath() const {
    return filePath_;
  }

  // Disable copying to avoid multiple removals.
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  // Enable move semantics.
  TempFile(TempFile&& other) noexcept : filePath_(std::move(other.filePath_)), autoRemove_(other.autoRemove_) {
    other.autoRemove_ = false; // Ensure the moved-from object won't delete the file.
  }

  TempFile& operator=(TempFile&& other) noexcept {
    if (this != &other) {
      filePath_ = std::move(other.filePath_);
      autoRemove_ = other.autoRemove_;
      other.autoRemove_ = false; // Prevent removal from the moved-from object.
    }
    return *this;
  }

 private:
  std::filesystem::path filePath_;
  bool autoRemove_;

  // Fallback function to generate a unique filename.
  std::string generate_unique_filename() {
    // Get a high-resolution timestamp.
    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    // Generate a random number.
    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_int_distribution<unsigned long long> dist;
    // Combine timestamp and random number.
    std::ostringstream oss;
    oss << "tempfile-" << now << "-" << dist(mt) << ".tmp";
    return oss.str();
  }
};

} // namespace utils
} // namespace hm
