#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/TransactionState.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <opencv2/imgcodecs.hpp>
#include <sys/stat.h>
#include <sys/wait.h>
#include <tiffio.h>
#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool write_tool(const std::filesystem::path& path, const std::string& body) {
  std::ofstream output(path);
  output << "#!/bin/sh\nset -eu\n" << body;
  output.close();
  std::error_code error;
  std::filesystem::permissions(
      path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace,
      error);
  return output.good() && !error;
}

bool write_spatial_tiff_tags(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    float x_position,
    float resolution) {
  TIFF* tif = TIFFOpen(path.c_str(), "w");
  if (tif == nullptr)
    return false;
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, height);
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, resolution);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, resolution);
  TIFFSetField(tif, TIFFTAG_XPOSITION, x_position);
  TIFFSetField(tif, TIFFTAG_YPOSITION, 0.0f);
  std::vector<unsigned char> row(width, 127);
  bool ok = true;
  for (uint32_t y = 0; y < height; ++y)
    ok &= TIFFWriteScanline(tif, row.data(), y, 0) >= 0;
  TIFFClose(tif);
  return ok;
}

bool write_remap_pair(
    const std::filesystem::path& directory,
    const std::string& prefix,
    int width,
    int height,
    bool degenerate = false) {
  cv::Mat x(height, width, CV_16U);
  cv::Mat y(height, width, CV_16U);
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) {
      x.at<uint16_t>(row, column) = static_cast<uint16_t>(degenerate ? 12 : column);
      y.at<uint16_t>(row, column) = static_cast<uint16_t>(degenerate ? 12 : row);
    }
  }
  return cv::imwrite((directory / (prefix + "_x.tif")).string(), x) &&
      cv::imwrite((directory / (prefix + "_y.tif")).string(), y);
}

uint32_t png_crc32(const unsigned char* data, size_t size) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return crc ^ 0xffffffffU;
}

void append_big_endian_u32(std::vector<unsigned char>* output, uint32_t value) {
  output->push_back(static_cast<unsigned char>(value >> 24));
  output->push_back(static_cast<unsigned char>(value >> 16));
  output->push_back(static_cast<unsigned char>(value >> 8));
  output->push_back(static_cast<unsigned char>(value));
}

bool add_png_pixel_offset(const std::filesystem::path& path, int32_t x, int32_t y) {
  std::ifstream input(path, std::ios::binary);
  std::vector<unsigned char> png((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (png.size() < 33 || std::string(png.begin() + 12, png.begin() + 16) != "IHDR")
    return false;
  std::vector<unsigned char> chunk;
  append_big_endian_u32(&chunk, 9);
  chunk.insert(chunk.end(), {'o', 'F', 'F', 's'});
  append_big_endian_u32(&chunk, static_cast<uint32_t>(x));
  append_big_endian_u32(&chunk, static_cast<uint32_t>(y));
  chunk.push_back(0); // pixel units
  append_big_endian_u32(&chunk, png_crc32(chunk.data() + 4, 13));
  png.insert(png.begin() + 33, chunk.begin(), chunk.end());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  return output.good();
}

std::vector<unsigned char> read_binary_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string generation_stat_identity(const std::filesystem::path& directory, bool portable) {
  static constexpr std::array<const char*, 10> names = {
      "hm_project.pto",
      "autooptimiser_out.pto",
      "mapping_0000.tif",
      "mapping_0000_x.tif",
      "mapping_0000_y.tif",
      "mapping_0001.tif",
      "mapping_0001_x.tif",
      "mapping_0001_y.tif",
      "seam_file.png",
      "stitching_canvas_provenance",
  };
  std::ostringstream identity;
  for (size_t index = 0; index < names.size(); ++index) {
    struct stat metadata{};
    if (::stat((directory / names[index]).c_str(), &metadata) != 0) {
      if (index + 1 == names.size() && errno == ENOENT)
        continue;
      return {};
    }
    identity << names[index] << ':';
    if (!portable) {
      identity << static_cast<uint64_t>(metadata.st_dev) << ':' << static_cast<uint64_t>(metadata.st_ino) << ':';
    }
    identity << static_cast<uint64_t>(metadata.st_size) << ':' << metadata.st_mtim.tv_sec << ':'
             << metadata.st_mtim.tv_nsec;
    if (!portable)
      identity << ':' << metadata.st_ctim.tv_sec << ':' << metadata.st_ctim.tv_nsec;
    identity << '\n';
  }
  return identity.str();
}

bool generation_matches_in_child(const std::filesystem::path& game_dir, const std::string& expected) {
  int descriptors[2] = {-1, -1};
  if (::pipe(descriptors) != 0)
    return false;
  const pid_t child = ::fork();
  if (child == 0) {
    ::close(descriptors[0]);
    auto lock = hm::stitching::HuginProject::RecoverAndLock(game_dir);
    const auto generation = lock.ok() ? hm::stitching::HuginProject::GenerationId(game_dir, **lock)
                                      : absl::StatusOr<std::string>(lock.status());
    size_t written = 0;
    while (generation.ok() && written < generation->size()) {
      const ssize_t count = ::write(descriptors[1], generation->data() + written, generation->size() - written);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
        break;
      written += static_cast<size_t>(count);
    }
    ::close(descriptors[1]);
    _exit(generation.ok() && written == generation->size() ? 0 : 1);
  }
  ::close(descriptors[1]);
  if (child < 0) {
    ::close(descriptors[0]);
    return false;
  }
  std::string actual;
  std::array<char, 2048> buffer{};
  while (true) {
    const ssize_t count = ::read(descriptors[0], buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    actual.append(buffer.data(), static_cast<size_t>(count));
  }
  ::close(descriptors[0]);
  int child_status = 0;
  while (::waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0 && actual == expected;
}

bool corrupt_png_pixel_offset_without_updating_crc(const std::filesystem::path& path) {
  std::vector<unsigned char> png = read_binary_file(path);
  const std::array<unsigned char, 4> type = {'o', 'F', 'F', 's'};
  const auto found = std::search(png.begin(), png.end(), type.begin(), type.end());
  if (found == png.end() || std::distance(found, png.end()) < 17)
    return false;
  *(found + 7) ^= 1U;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  return output.good();
}

bool move_png_pixel_offset_after_first_image_data(const std::filesystem::path& path) {
  std::vector<unsigned char> png = read_binary_file(path);
  const std::array<unsigned char, 4> offset_type = {'o', 'F', 'F', 's'};
  auto offset = std::search(png.begin(), png.end(), offset_type.begin(), offset_type.end());
  if (offset == png.end() || std::distance(png.begin(), offset) < 4 || std::distance(offset, png.end()) < 17)
    return false;
  auto chunk_begin = offset - 4;
  std::vector<unsigned char> chunk(chunk_begin, chunk_begin + 21);
  png.erase(chunk_begin, chunk_begin + 21);

  const std::array<unsigned char, 4> image_data_type = {'I', 'D', 'A', 'T'};
  const auto image_data = std::search(png.begin(), png.end(), image_data_type.begin(), image_data_type.end());
  if (image_data == png.end() || std::distance(png.begin(), image_data) < 4)
    return false;
  const auto length = static_cast<size_t>(static_cast<uint32_t>(*(image_data - 4)) << 24) |
      static_cast<size_t>(static_cast<uint32_t>(*(image_data - 3)) << 16) |
      static_cast<size_t>(static_cast<uint32_t>(*(image_data - 2)) << 8) |
      static_cast<size_t>(static_cast<uint32_t>(*(image_data - 1)));
  if (length > static_cast<size_t>(std::distance(image_data, png.end())) - 8)
    return false;
  png.insert(image_data + 8 + static_cast<std::ptrdiff_t>(length), chunk.begin(), chunk.end());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  return output.good();
}

} // namespace

int main() {
  ::setenv("HM_TEST_FORCE_TRANSACTION_RECOVERY_SCAN", "1", 1);
  bool ok = true;
  std::vector<hm::stitching::FeatureMatch> matches;
  for (int i = 0; i < 16; ++i) {
    matches.push_back({{i + 0.25f, i + 1.5f}, {i + 2.75f, i + 3.125f}, 0.9f});
  }
  const std::string base =
      "# hugin project file\n"
      "p f2 w12092 h9267 v360\n"
      "# control points\n"
      "c n0 N1 x999 y999 X999 Y999 t0\n"
      "#hugin_optimizeReferenceImage 0\n";
  auto inserted = hm::stitching::HuginProject::InsertControlPoints(base, matches);
  ok &= expect(inserted.ok(), "valid control points must insert");
  if (inserted.ok()) {
    ok &= expect(
        inserted->find("x0.25 y1.5 X2.75 Y3.125 t0") != std::string::npos, "PTO decimals must be locale independent");
    ok &= expect(inserted->find("x999") == std::string::npos, "old PTO control points must be removed");
    size_t count = 0;
    for (size_t at = 0; (at = inserted->find("\nc n0 N1 ", at)) != std::string::npos; at += 2)
      ++count;
    ok &= expect(count == matches.size(), "every selected match must produce one control point");
  }
  auto canvas = hm::stitching::HuginProject::ParseCanvasSize(base);
  ok &= expect(canvas.ok() && canvas->first == 12092 && canvas->second == 9267, "PTO canvas dimensions must parse");
  auto projection = hm::stitching::HuginProject::ParseProjection(base);
  ok &= expect(projection.ok() && *projection == 2, "PTO panorama projection must parse");
  auto horizontal_fov = hm::stitching::HuginProject::ParseHorizontalFov(base);
  ok &= expect(horizontal_fov.ok() && *horizontal_fov == 360.0, "PTO panorama horizontal FOV must parse");
  ok &= expect(!hm::stitching::HuginProject::ParseProjection("p w12 h6\n").ok(), "missing projection must fail");
  ok &= expect(!hm::stitching::HuginProject::ParseHorizontalFov("p f2 w12 h6\n").ok(), "missing FOV must fail");
  ok &= expect(!hm::stitching::HuginProject::ParseCanvasSize("p f2 w12 v360\n").ok(), "missing height must fail");
  const std::string pose_project =
      "i w5312 h2988 f0 r-1.25e-2 p2.5 y-43.75 Tpy0 n\"left.png\"\n"
      "i w5312 h2988 f0 r=0 p=0 y=0 r0.125 p-4.25 y43.5 n\"right.png\"\n";
  auto pose = hm::stitching::HuginProject::ParseCameraPose(pose_project, 1);
  ok &= expect(
      pose.ok() && std::abs(pose->roll - 0.125) < 1e-12 && std::abs(pose->pitch + 4.25) < 1e-12 &&
          std::abs(pose->yaw - 43.5) < 1e-12,
      "PTO camera pose must parse without confusing linked or translation tokens");
  ok &= expect(
      !hm::stitching::HuginProject::ParseCameraPose("i w1 h1 rnan p0 y0 n\"bad.png\"\n", 0).ok(),
      "non-finite PTO camera pose must fail");
  ok &=
      expect(!hm::stitching::HuginProject::ParseCameraPose(pose_project, 2).ok(), "missing PTO image index must fail");
  matches.resize(15);
  ok &=
      expect(!hm::stitching::HuginProject::InsertControlPoints(base, matches).ok(), "too few control points must fail");
  matches.resize(16, hm::stitching::FeatureMatch{{0.0f, 0.0f}, {1.0f, 1.0f}, 1.0f});
  matches[0].left.x = std::nanf("");
  ok &=
      expect(!hm::stitching::HuginProject::InsertControlPoints(base, matches).ok(), "non-finite coordinates must fail");

  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("hstream-hugin-test-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "game");
  ok &= expect(
      cv::imwrite((root / "game" / "left.png").string(), cv::Mat(48, 64, CV_8UC3, cv::Scalar(1, 2, 3))),
      "left source fixture must exist");
  ok &= expect(
      cv::imwrite((root / "game" / "right.png").string(), cv::Mat(48, 64, CV_8UC3, cv::Scalar(4, 5, 6))),
      "right source fixture must exist");
  const fs::path pto_gen = root / "pto_gen";
  const fs::path pto_gen_args = root / "pto_gen.args";
  const fs::path autooptimiser = root / "autooptimiser";
  const fs::path autooptimiser_args = root / "autooptimiser.args";
  const fs::path pano_modify = root / "pano_modify";
  const fs::path pano_modify_args = root / "pano_modify.args";
  const fs::path nona = root / "nona";
  const fs::path nona_invocations = root / "nona.invocations";
  const fs::path enblend = root / "enblend";
  const fs::path enblend_invocations = root / "enblend.invocations";
  const fs::path fixtures = root / "fixtures";
  const fs::path rounding_overflow_fixtures = root / "rounding-overflow-fixtures";
  fs::create_directories(fixtures);
  fs::create_directories(rounding_overflow_fixtures);
  constexpr float boundary_resolution = 8033.26416015625f;
  ok &= expect(
      write_spatial_tiff_tags(fixtures / "mapping_0000.tif", 40, 32, 3490.974853515625f, boundary_resolution),
      "first fake mapping must exist");
  ok &= expect(
      write_spatial_tiff_tags(fixtures / "mapping_0001.tif", 40, 32, 3490.97509765625f, boundary_resolution),
      "second fake mapping must exist");
  ok &= expect(write_remap_pair(fixtures, "mapping_0000", 40, 32), "first fake CV_16U remap must exist");
  ok &= expect(write_remap_pair(fixtures, "mapping_0001", 40, 32), "second fake CV_16U remap must exist");
  ok &= expect(
      write_spatial_tiff_tags(rounding_overflow_fixtures / "mapping_0000.tif", 32, 32, 0.0f, 1.0f) &&
          write_spatial_tiff_tags(rounding_overflow_fixtures / "mapping_0001.tif", 33, 32, 32.0f, 1.0f) &&
          write_remap_pair(rounding_overflow_fixtures, "mapping_0000", 32, 32) &&
          write_remap_pair(rounding_overflow_fixtures, "mapping_0001", 33, 32),
      "rounding overflow fixtures must describe a 65-pixel canvas");
  cv::Mat seam(30, 40, CV_8U, cv::Scalar(0));
  seam.colRange(20, 40).setTo(255);
  ok &= expect(cv::imwrite((fixtures / "seam_file.png").string(), seam), "fake seam must exist");
  ok &= expect(
      add_png_pixel_offset(fixtures / "seam_file.png", 1, 1), "fake cropped enblend seam must carry an oFFs origin");

  const fs::path seam_validation = root / "seam-validation";
  fs::create_directories(seam_validation);
  const fs::path corrupt_offset = seam_validation / "corrupt-offset.png";
  ok &= expect(cv::imwrite(corrupt_offset.string(), seam), "corrupt-offset fixture must be encoded");
  ok &= expect(
      add_png_pixel_offset(corrupt_offset, 1, 1) && corrupt_png_pixel_offset_without_updating_crc(corrupt_offset),
      "corrupt-offset fixture must retain a stale oFFs CRC");
  ok &= expect(
      absl::IsFailedPrecondition(hm::stitching::HuginProject::ValidateAndNormalizeSeam(corrupt_offset, 42, 32)),
      "a corrupt oFFs payload must fail closed even when libpng can decode the pixels");

  const fs::path duplicate_offset = seam_validation / "duplicate-offset.png";
  ok &= expect(cv::imwrite(duplicate_offset.string(), seam), "duplicate-offset fixture must be encoded");
  ok &= expect(
      add_png_pixel_offset(duplicate_offset, 1, 1) && add_png_pixel_offset(duplicate_offset, 1, 1),
      "duplicate-offset fixture must contain two valid oFFs chunks");
  ok &= expect(
      absl::IsFailedPrecondition(hm::stitching::HuginProject::ValidateAndNormalizeSeam(duplicate_offset, 42, 32)),
      "duplicate oFFs chunks must fail closed");

  const fs::path late_offset = seam_validation / "late-offset.png";
  ok &= expect(cv::imwrite(late_offset.string(), seam), "late-offset fixture must be encoded");
  ok &= expect(
      add_png_pixel_offset(late_offset, 1, 1) && move_png_pixel_offset_after_first_image_data(late_offset),
      "late-offset fixture must carry its oFFs chunk after IDAT");
  ok &= expect(
      absl::IsFailedPrecondition(hm::stitching::HuginProject::ValidateAndNormalizeSeam(late_offset, 42, 32)),
      "an oFFs chunk after image data must fail closed instead of being ignored");

  const fs::path interrupted_normalization = seam_validation / "interrupted-normalization.png";
  ok &= expect(
      cv::imwrite(interrupted_normalization.string(), seam) && add_png_pixel_offset(interrupted_normalization, 1, 1),
      "interrupted-normalization fixture must carry an oFFs origin");
  const std::vector<unsigned char> original_interrupted_seam = read_binary_file(interrupted_normalization);
  ::setenv("HM_TEST_SEAM_NORMALIZATION_FAIL_BEFORE_RENAME", "1", 1);
  const auto interrupted_normalization_status =
      hm::stitching::HuginProject::ValidateAndNormalizeSeam(interrupted_normalization, 42, 32);
  ::unsetenv("HM_TEST_SEAM_NORMALIZATION_FAIL_BEFORE_RENAME");
  bool temporary_remains = false;
  for (const auto& entry : fs::directory_iterator(seam_validation)) {
    if (entry.path().filename().string().rfind(".interrupted-normalization.png.normalize-", 0) == 0)
      temporary_remains = true;
  }
  ok &= expect(
      !interrupted_normalization_status.ok() &&
          read_binary_file(interrupted_normalization) == original_interrupted_seam && !temporary_remains,
      "failed normalization must preserve the published seam and remove its temporary file");
  ok &= expect(
      hm::stitching::HuginProject::ValidateAndNormalizeSeam(interrupted_normalization, 42, 32).ok() &&
          cv::imread(interrupted_normalization.string(), cv::IMREAD_GRAYSCALE).size() == cv::Size(42, 32),
      "successful normalization must atomically publish the full-canvas seam");

  const fs::path missing_offset_edge = seam_validation / "missing-offset-edge.png";
  cv::Mat one_pixel_short_seam(31, 42, CV_8U, cv::Scalar(0));
  one_pixel_short_seam.colRange(21, 42).setTo(255);
  ok &= expect(cv::imwrite(missing_offset_edge.string(), one_pixel_short_seam), "edge-cropped seam must be encoded");
  const auto missing_offset_status =
      hm::stitching::HuginProject::ValidateAndNormalizeSeam(missing_offset_edge, 42, 32);
  const cv::Mat normalized_missing_offset = cv::imread(missing_offset_edge.string(), cv::IMREAD_GRAYSCALE);
  cv::Mat expected_missing_offset;
  cv::copyMakeBorder(one_pixel_short_seam, expected_missing_offset, 0, 1, 0, 0, cv::BORDER_REPLICATE);
  ok &= expect(
      missing_offset_status.ok() && normalized_missing_offset.size() == cv::Size(42, 32) &&
          cv::norm(normalized_missing_offset, expected_missing_offset, cv::NORM_INF) == 0,
      "one-pixel origin-zero enblend seam without oFFs metadata must be normalized");

  const fs::path missing_offset_large = seam_validation / "missing-offset-large.png";
  cv::Mat large_mismatch_seam(30, 40, CV_8U, cv::Scalar(0));
  large_mismatch_seam.colRange(20, 40).setTo(255);
  ok &= expect(cv::imwrite(missing_offset_large.string(), large_mismatch_seam), "large mismatched seam must be encoded");
  ok &= expect(
      absl::IsFailedPrecondition(hm::stitching::HuginProject::ValidateAndNormalizeSeam(missing_offset_large, 42, 32)),
      "larger origin-zero enblend seam mismatch without oFFs metadata must still fail closed");
  ok &= expect(
      cv::imwrite((fixtures / "panorama.tif").string(), cv::Mat(32, 42, CV_8UC3, cv::Scalar(1, 2, 3))),
      "fake panorama must exist");
  ok &= expect(
      write_tool(
          pto_gen,
          "printf '%s\\n' \"$*\" > '" + pto_gen_args.string() +
              "'\n"
              "printf '%s\\n' '# hugin project file' 'p f2 w100 h50 v180 n\"TIFF_m c:LZW r:CROP\"' '# control points' "
              "'#hugin_optimizeReferenceImage 0' > hm_project.pto\n"),
      "fake pto_gen must be created");
  ok &= expect(
      write_tool(
          autooptimiser,
          "args=\"$*\"; align=0; level=0; select=0; quiet=0; output=; input=\n"
          "while [ \"$#\" -gt 0 ]; do case \"$1\" in -a) align=1 ;; -l) level=1 ;; -s) select=1 ;; "
          "-q) quiet=1 ;; -x|-n) exit 91 ;; -o) shift; output=$1 ;; *) input=$1 ;; "
          "esac; shift; done\n"
          "test \"$align:$level:$select:$quiet:$output:$input\" = "
          "'1:1:1:1:autooptimiser_out.pto:hm_project.pto'\n"
          "awk '/^p / { sub(/w[0-9]+/, \"w100\"); sub(/h[0-9]+/, \"h50\") } { print }' "
          "\"$input\" > \"$output\"\n"
          "printf '%s\\n' \"$args\" >> '" +
              autooptimiser_args.string() +
              "'\n"
              "printf '%s\\n' 'Average (rms) distance between Controlpoints' 'after 1 iteration(s): 1.25 units'\n"),
      "fake autooptimiser must be created");
  ok &= expect(
      write_tool(
          pano_modify,
          "args=\"$*\"; projection=; parameters=; fov=; canvas=; crop=0; output=; input=\n"
          "while [ \"$#\" -gt 0 ]; do case \"$1\" in --projection=*) projection=${1#*=} ;; "
          "--projection-parameter=*) parameters=${1#*=} ;; --canvas=*) canvas=${1#--canvas=} ;; "
          "--fov=*) fov=${1#*=} ;; --crop=AUTO) crop=1 ;; --crop=0,100,0,100%) crop=0 ;; "
          "--crop=*) exit 93 ;; --output=*) output=${1#*=} ;; "
          "-o) shift; output=$1 ;; *) input=$1 ;; esac; shift; done\n"
          "test \"$input\" = autooptimiser_out.pto\n"
          "if test -n \"${HM_TEST_PANO_FOV_CLAMP:-}\" && test \"$fov\" != AUTO; then "
          "fov=$HM_TEST_PANO_FOV_CLAMP; fi\n"
          "if test \"$projection\" = 19; then\n"
          "  test -n \"$fov\"\n"
          "  if test -n \"${HM_TEST_PANINI_PARAMETERS:-}\"; then parameters=$HM_TEST_PANINI_PARAMETERS; fi\n"
          "  test -n \"$parameters\"\n"
          "  if test \"$canvas\" = AUTO; then width=200; height=100; elif test -z \"$canvas\"; then "
          "width=62; height=31; else "
          "case \"$canvas\" in *%) ;; *) exit 92 ;; esac; width=62; height=31; fi\n"
          "  awk -v parameters=\"$parameters\" -v fov=\"$fov\" -v width=$width -v height=$height -v crop=$crop '/^p / { "
          "sub(/f[0-9]+/, \"f19\"); sub(/w[0-9]+/, \"w\" width); sub(/h[0-9]+/, \"h\" height); "
          "if (fov != \"AUTO\") sub(/v[-+0-9.eE]+/, \"v\" fov); "
          "if (crop) $0 = $0 \" S0,\" width \",0,\" height; $0 = $0 \" P\\\"\" parameters \"\\\"\" } { print }' "
          "\"$input\" > \"$output\"\n"
          "elif test -n \"$projection\"; then\n"
          "  test -n \"$fov\"\n"
          "  if test \"$canvas\" = AUTO; then width=100000; height=50000; elif test -z \"$canvas\"; then "
          "width=62; height=31; else "
          "case \"$canvas\" in *x*) width=${canvas%x*}; height=${canvas#*x} ;; *) exit 92 ;; esac; fi\n"
          "  awk -v projection=\"$projection\" -v fov=\"$fov\" -v width=$width -v height=$height -v crop=$crop '/^p / { "
          "sub(/f[0-9]+/, \"f\" projection); sub(/w[0-9]+/, \"w\" width); sub(/h[0-9]+/, \"h\" height); "
          "if (fov != \"AUTO\") sub(/v[-+0-9.eE]+/, \"v\" fov); "
          "if (crop) $0 = $0 \" S0,\" width \",0,\" height } { print }' \"$input\" > \"$output\"\n"
          "else\n"
          "  test \"$output\" = .autooptimiser_out.resize.pto\n"
          "  width=${canvas%x*}; height=${canvas#*x}\n"
          "  if test -n \"${HM_TEST_PANO_ROUND_WIDTH:-}\"; then width=$((width + 1)); fi\n"
          "  awk -v width=\"$width\" -v height=\"$height\" "
          "'/^p / { sub(/w[0-9]+/, \"w\" width); sub(/h[0-9]+/, \"h\" height) } { print }' "
          "\"$input\" > \"$output\"\n"
          "fi\n"
          "printf '%s\\n' \"$args\" >> '" +
              pano_modify_args.string() + "'\n"),
      "fake pano_modify must be created");
  ok &= expect(
      write_tool(
          nona,
          "printf '%s\\n' run >> '" + nona_invocations.string() +
              "'\n"
              "fixtures='" +
              fixtures.string() +
              "'\n"
              "if grep -q ' w64 ' autooptimiser_out.pto; then fixtures='" +
              rounding_overflow_fixtures.string() +
              "'; fi\n"
              "for file in mapping_0000.tif mapping_0000_x.tif mapping_0000_y.tif mapping_0001.tif "
              "mapping_0001_x.tif mapping_0001_y.tif; do cp \"$fixtures/$file\" \"$file\"; done\n"),
      "fake nona must be created");
  ok &= expect(
      write_tool(
          enblend,
          "printf '%s\\n' run >> '" + enblend_invocations.string() +
              "'\n"
              "cp '" +
              fixtures.string() +
              "/seam_file.png' seam_file.png\n"
              "cp '" +
              fixtures.string() + "/panorama.tif' panorama.tif\n"),
      "fake enblend must be created");
  ::setenv("HM_PTO_GEN", pto_gen.c_str(), 1);
  ::setenv("HM_AUTOOPTIMISER", autooptimiser.c_str(), 1);
  ::setenv("HM_PANO_MODIFY", pano_modify.c_str(), 1);
  ::setenv("HM_NONA", nona.c_str(), 1);
  ::setenv("HM_ENBLEND", enblend.c_str(), 1);
  matches.clear();
  for (int i = 0; i < 16; ++i) {
    matches.push_back({{i + 0.25f, i + 1.5f}, {i + 2.75f, i + 3.125f}, 0.9f});
  }
  hm::stitching::HuginProject::Options options;
  options.max_canvas_dimension = 64;
  options.mapping_backend = hm::stitching::MappingBackend::kNona;
  options.run_autooptimizer = true;
  options.projection_framing = {true, 180.0, true, true};
  fs::create_directories(root / "private-inputs");
  ok &= expect(
      cv::imwrite((root / "private-inputs" / "left.png").string(), cv::Mat(48, 64, CV_8UC3, cv::Scalar(11, 12, 13))),
      "private left calibration input must exist");
  ok &= expect(
      cv::imwrite((root / "private-inputs" / "right.png").string(), cv::Mat(48, 64, CV_8UC3, cv::Scalar(21, 22, 23))),
      "private right calibration input must exist");
  std::vector<std::string> failure_progress;
  options.progress = [&failure_progress](const std::string& stage, const std::string& status, const std::string&) {
    failure_progress.push_back(stage + ":" + status);
  };
  ::setenv("HM_PTO_GEN", (root / "missing-pto-gen").c_str(), 1);
  const auto missing_optimizer_setup = hm::stitching::HuginProject::Configure(
      root / "game", root / "private-inputs" / "left.png", root / "private-inputs" / "right.png", matches, options);
  ok &= expect(!missing_optimizer_setup.ok(), "missing Hugin setup tool must fail calibration");
  ok &= expect(
      !failure_progress.empty() && failure_progress.front() == "optimizer:started",
      "optimizer stage must become active before Hugin setup can fail");
  ::setenv("HM_PTO_GEN", pto_gen.c_str(), 1);
  options.progress = {};
  const fs::path slow_autooptimiser = root / "slow-autooptimiser";
  const fs::path slow_optimizer_started = root / "slow-autooptimiser.started";
  ok &= expect(
      write_tool(
          slow_autooptimiser,
          "printf started > '" + slow_optimizer_started.string() +
              "'\n"
              "sleep 30\n"),
      "slow fake autooptimiser must be created");
  ::setenv("HM_AUTOOPTIMISER", slow_autooptimiser.c_str(), 1);
  std::atomic<bool> cancel_optimizer{false};
  options.is_cancelled = [&] { return cancel_optimizer.load(); };
  std::thread optimizer_interrupt([&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!fs::exists(slow_optimizer_started) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    cancel_optimizer = true;
  });
  const auto cancellation_started = std::chrono::steady_clock::now();
  const auto cancelled_optimizer = hm::stitching::HuginProject::Configure(
      root / "game", root / "private-inputs" / "left.png", root / "private-inputs" / "right.png", matches, options);
  const auto cancellation_elapsed = std::chrono::steady_clock::now() - cancellation_started;
  optimizer_interrupt.join();
  options.is_cancelled = {};
  ::setenv("HM_AUTOOPTIMISER", autooptimiser.c_str(), 1);
  ok &= expect(
      absl::IsCancelled(cancelled_optimizer) && cancellation_elapsed < std::chrono::seconds(5),
      "optimizer cancellation must terminate the Hugin process group before pipeline shutdown times out");
  options.projection = hm::stitching::StitchProjection::kGeneralPanini;
  const auto configured = hm::stitching::HuginProject::Configure(
      root / "game", root / "private-inputs" / "left.png", root / "private-inputs" / "right.png", matches, options);
  if (!configured.ok())
    std::cerr << configured << '\n';
  ok &= expect(configured.ok(), "fake Hugin toolchain must complete orchestration");
  if (configured.ok()) {
    std::ifstream pto_gen_invocation(pto_gen_args);
    const std::string pto_gen_arguments(
        (std::istreambuf_iterator<char>(pto_gen_invocation)), std::istreambuf_iterator<char>());
    ok &= expect(
        pto_gen_arguments == "-p 0 -o hm_project.pto -f 108 left.png right.png\n",
        "Hugin input images must be declared rectilinear like HockeyMOM's known-good calibration path");
    std::ifstream optimized(root / "game" / "autooptimiser_out.pto");
    const std::string contents((std::istreambuf_iterator<char>(optimized)), std::istreambuf_iterator<char>());
    const auto scaled = hm::stitching::HuginProject::ParseCanvasSize(contents);
    ok &= expect(
        scaled.ok() && scaled->first == 63 && scaled->second == 32,
        "explicit max canvas dimension must resize General Panini AUTO geometry after projection conversion");
    ok &= expect(
        contents.find("p f19 ") != std::string::npos && contents.find("P\"100 0 0\"") != std::string::npos,
        "single-pass calibration must publish the selected General Panini projection");
    std::ifstream optimizer_invocations(autooptimiser_args);
    const std::string optimizer_args(
        (std::istreambuf_iterator<char>(optimizer_invocations)), std::istreambuf_iterator<char>());
    ok &= expect(
        optimizer_args.find("-a -l -s -q -o autooptimiser_out.pto hm_project.pto") != std::string::npos,
        "Hugin orchestration must request automatic alignment and projection selection");
    ok &= expect(
        std::count(optimizer_args.begin(), optimizer_args.end(), '\n') == 1 &&
            optimizer_args.find("-x") == std::string::npos && optimizer_args.find("-n") == std::string::npos,
        "Hugin orchestration must optimize once without requesting unsupported output scaling");
    const std::string pano_modify_invocations = read_text_file(pano_modify_args);
    ok &= expect(
        pano_modify_invocations.find("--projection=19 --projection-parameter=100 0 0") != std::string::npos &&
            pano_modify_invocations.find("--fov=AUTO --canvas=AUTO --crop=AUTO") != std::string::npos &&
            pano_modify_invocations.find("--canvas=63x32 -o .autooptimiser_out.resize.pto autooptimiser_out.pto") !=
                std::string::npos,
        "General Panini conversion must preserve automatic geometry until the explicit max-dimension cap runs");
    const std::string nona_runs = read_text_file(nona_invocations);
    ok &= expect(
        std::count(nona_runs.begin(), nona_runs.end(), '\n') == 1,
        "Nona must render the selected projection exactly once");
    const std::string enblend_runs = read_text_file(enblend_invocations);
    ok &= expect(
        std::count(enblend_runs.begin(), enblend_runs.end(), '\n') == 1,
        "enblend must stitch the selected projection exactly once");
    const cv::Mat published_seam = cv::imread((root / "game" / "seam_file.png").string(), cv::IMREAD_GRAYSCALE);
    cv::Mat expected_seam;
    cv::copyMakeBorder(seam, expected_seam, 1, 1, 1, 1, cv::BORDER_REPLICATE);
    ok &= expect(
        published_seam.size() == cv::Size(42, 32) && cv::norm(published_seam, expected_seam, cv::NORM_INF) == 0,
        "Hugin validation must place an oFFs-cropped enblend mask onto hm-cupano's full mapping canvas");
    const cv::Mat published_left = cv::imread((root / "game" / "left.png").string(), cv::IMREAD_COLOR);
    const cv::Mat published_right = cv::imread((root / "game" / "right.png").string(), cv::IMREAD_COLOR);
    ok &= expect(
        !published_left.empty() && published_left.at<cv::Vec3b>(0, 0) == cv::Vec3b(11, 12, 13),
        "Hugin publication must atomically install its private left input");
    ok &= expect(
        !published_right.empty() && published_right.at<cv::Vec3b>(0, 0) == cv::Vec3b(21, 22, 23),
        "Hugin publication must atomically install its private right input");
  }
  for (const char* artifact : {
           "hm_project.pto",
           "autooptimiser_out.pto",
           "mapping_0000.tif",
           "mapping_0000_x.tif",
           "mapping_0000_y.tif",
           "mapping_0001.tif",
           "mapping_0001_x.tif",
           "mapping_0001_y.tif",
           "seam_file.png",
           "panorama.tif",
           "left.png",
           "right.png",
           "stitching_canvas_provenance",
       }) {
    ok &= expect(fs::is_regular_file(root / "game" / artifact), "configured Hugin artifact must be published");
  }
  auto provenance_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
  ok &= expect(provenance_lock.ok(), "published Hugin provenance must be readable under the artifact lock");
  if (provenance_lock.ok()) {
    const auto provenance = hm::stitching::HuginProject::ReadCanvasProvenance(root / "game", **provenance_lock);
    ok &= expect(
        provenance.ok() && provenance->has_value() && (*provenance)->max_output_width == 0 &&
            (*provenance)->max_canvas_dimension == 64 && (*provenance)->source_canvas_width == 200 &&
            (*provenance)->source_canvas_height == 102 && (*provenance)->canvas_width == 42 &&
            (*provenance)->canvas_height == 32 && !(*provenance)->max_output_width_applied &&
            (*provenance)->max_canvas_dimension_applied &&
            (*provenance)->mapping_backend == hm::stitching::MappingBackend::kNona &&
            (*provenance)->projection == hm::stitching::StitchProjection::kGeneralPanini &&
            (*provenance)->projection_parameters == std::vector<double>({100.0, 0.0, 0.0}) &&
            (*provenance)->projection_framing == options.projection_framing &&
            (*provenance)->control_point_matcher == hm::stitching::ControlPointMatcher::kSuperPointLightGlue &&
            (*provenance)->akaze_calibration_fingerprint == "not-applicable",
        "published Hugin provenance must record canvas, matcher, calibration, algorithm, parameters, and framing");
    provenance_lock->reset();
  }

  const fs::path custom_panini = root / "custom-panini";
  fs::create_directories(custom_panini);
  std::error_code custom_copy_error;
  fs::copy_file(
      root / "game" / "hm_project.pto",
      custom_panini / "autooptimiser_out.pto",
      fs::copy_options::overwrite_existing,
      custom_copy_error);
  ok &= expect(!custom_copy_error, "custom Panini fixture PTO must be copied");
  const auto custom_parameters = hm::stitching::HuginProject::ApplyProjection(
      custom_panini, hm::stitching::StitchProjection::kGeneralPanini, {120.0, 25.0, -40.0});
  ok &= expect(custom_parameters.ok(), "custom General Panini parameters must be accepted");
  const std::string custom_panini_pto = read_text_file(custom_panini / "autooptimiser_out.pto");
  ok &= expect(
      custom_panini_pto.find("P\"120 25 -40\"") != std::string::npos,
      "pano_modify must receive and preserve the selected General Panini values");
  const auto unsupported_parameter_precision = hm::stitching::HuginProject::ApplyProjection(
      custom_panini, hm::stitching::StitchProjection::kGeneralPanini, {100.123456789, 0.0, 0.0});
  ok &= expect(
      absl::IsInvalidArgument(unsupported_parameter_precision) &&
          unsupported_parameter_precision.message().find("increments of 0.01") != std::string::npos,
      "projection conversion must reject precision that pano_modify cannot preserve before invoking Hugin");

  std::ofstream(pano_modify_args, std::ios::trunc).close();
  for (unsigned mask = 0; mask < 8; ++mask) {
    const fs::path framing_dir = root / ("framing-" + std::to_string(mask));
    fs::create_directories(framing_dir);
    std::error_code framing_copy_error;
    fs::copy_file(
        root / "game" / "hm_project.pto",
        framing_dir / "autooptimiser_out.pto",
        fs::copy_options::overwrite_existing,
        framing_copy_error);
    const hm::stitching::StitchProjectionFraming framing{
        (mask & 1U) != 0, 185.0, (mask & 2U) != 0, (mask & 4U) != 0};
    const auto framed = framing_copy_error
        ? absl::Status(absl::StatusCode::kInternal, framing_copy_error.message())
        : hm::stitching::HuginProject::ApplyProjection(
              framing_dir,
              hm::stitching::StitchProjection::kGeneralPanini,
              {100.0, 0.0, 0.0},
              framing);
    const std::string expected_arguments =
        std::string("--projection=19 --projection-parameter=100 0 0 --fov=") +
        (framing.auto_fov ? "AUTO" : "185") + (framing.auto_canvas ? " --canvas=AUTO" : "") +
        (framing.auto_crop ? " --crop=AUTO" : " --crop=0,100,0,100%") +
        " --output=.autooptimiser_out.projection.pto";
    const std::string framed_project = read_text_file(framing_dir / "autooptimiser_out.pto");
    ok &= expect(
        framed.ok() && read_text_file(pano_modify_args).find(expected_arguments) != std::string::npos &&
            (framed_project.find(" S") != std::string::npos) == framing.auto_crop,
        "all Auto FOV/Canvas/Crop permutations must produce exact arguments and an explicit full canvas when crop is off");
  }

  const fs::path rounded_fixed_fov = root / "rounded-fixed-fov";
  fs::create_directories(rounded_fixed_fov);
  std::error_code rounded_fixed_fov_copy_error;
  fs::copy_file(
      root / "game" / "hm_project.pto",
      rounded_fixed_fov / "autooptimiser_out.pto",
      fs::copy_options::overwrite_existing,
      rounded_fixed_fov_copy_error);
  ::setenv("HM_TEST_PANO_FOV_CLAMP", "185.123", 1);
  const auto rounded_fixed_fov_status = hm::stitching::HuginProject::ApplyProjection(
      rounded_fixed_fov,
      hm::stitching::StitchProjection::kGeneralPanini,
      {100.0, 0.0, 0.0},
      hm::stitching::StitchProjectionFraming{false, 185.123456789, true, false});
  ::unsetenv("HM_TEST_PANO_FOV_CLAMP");
  ok &= expect(
      !rounded_fixed_fov_copy_error && rounded_fixed_fov_status.ok(),
      "fixed FOV validation must accept pano_modify's three-decimal PTO serialization");

  const fs::path unsupported_rectilinear = root / "unsupported-rectilinear-fov";
  fs::create_directories(unsupported_rectilinear);
  std::error_code rectilinear_copy_error;
  fs::copy_file(
      root / "game" / "hm_project.pto",
      unsupported_rectilinear / "autooptimiser_out.pto",
      fs::copy_options::overwrite_existing,
      rectilinear_copy_error);
  ::setenv("HM_TEST_PANO_FOV_CLAMP", "179", 1);
  const auto unsupported_fov = hm::stitching::HuginProject::ApplyProjection(
      unsupported_rectilinear,
      hm::stitching::StitchProjection::kRectilinear,
      {},
      hm::stitching::StitchProjectionFraming{false, 180.0, false, false});
  ::unsetenv("HM_TEST_PANO_FOV_CLAMP");
  ok &= expect(
      !rectilinear_copy_error && absl::IsInvalidArgument(unsupported_fov) &&
          unsupported_fov.message().find("at most 179 degrees") != std::string::npos,
      "Hugin must reject an unsupported fixed Rectilinear field of view before invoking pano_modify");

  const fs::path extreme_stereographic = root / "extreme-stereographic";
  fs::create_directories(extreme_stereographic);
  std::error_code extreme_copy_error;
  fs::copy_file(
      root / "game" / "hm_project.pto",
      extreme_stereographic / "autooptimiser_out.pto",
      fs::copy_options::overwrite_existing,
      extreme_copy_error);
  ok &= expect(!extreme_copy_error, "extreme Stereographic fixture PTO must be copied");
  const auto extreme_projection = hm::stitching::HuginProject::ApplyProjection(
      extreme_stereographic, hm::stitching::StitchProjection::kStereographic);
  ok &= expect(extreme_projection.ok(), "an AUTO projection must preserve Hugin's projection canvas");
  const std::string extreme_stereographic_pto = read_text_file(extreme_stereographic / "autooptimiser_out.pto");
  const auto extreme_stereographic_canvas = hm::stitching::HuginProject::ParseCanvasSize(extreme_stereographic_pto);
  ok &= expect(
      extreme_stereographic_canvas.ok() && extreme_stereographic_canvas->first == 100000 &&
          extreme_stereographic_canvas->second == 50000 &&
          read_text_file(pano_modify_args).find("--projection=4 --fov=AUTO --canvas=AUTO --crop=AUTO") !=
              std::string::npos,
      "Stereographic conversion must keep Hugin's automatic canvas");

  const fs::path nonfinite_panini = root / "nonfinite-panini";
  fs::create_directories(nonfinite_panini);
  std::error_code copy_error;
  fs::copy_file(
      root / "game" / "hm_project.pto",
      nonfinite_panini / "autooptimiser_out.pto",
      fs::copy_options::overwrite_existing,
      copy_error);
  ok &= expect(!copy_error, "non-finite Panini fixture PTO must be copied");
  ::setenv("HM_TEST_PANINI_PARAMETERS", "nan 0 0", 1);
  const auto nonfinite_parameters =
      hm::stitching::HuginProject::ApplyProjection(nonfinite_panini, hm::stitching::StitchProjection::kGeneralPanini);
  ::unsetenv("HM_TEST_PANINI_PARAMETERS");
  ok &= expect(
      absl::IsFailedPrecondition(nonfinite_parameters),
      "non-finite parameters emitted by pano_modify must fail before remap publication");
  options.projection.reset();

  const fs::path width_headroom_game = root / "nona-width-headroom-game";
  fs::create_directories(width_headroom_game);
  hm::stitching::HuginProject::Options width_headroom_options = options;
  width_headroom_options.max_canvas_dimension.reset();
  width_headroom_options.max_output_width = 64;
  width_headroom_options.progress = {};
  const auto width_headroom_configured = hm::stitching::HuginProject::Configure(
      width_headroom_game,
      root / "private-inputs" / "left.png",
      root / "private-inputs" / "right.png",
      matches,
      width_headroom_options);
  ok &= expect(width_headroom_configured.ok(), "Nona width cap must complete with initial placement headroom");
  const std::string width_headroom_nona_runs = read_text_file(nona_invocations);
  ok &= expect(
      std::count(width_headroom_nona_runs.begin(), width_headroom_nona_runs.end(), '\n') == 2,
      "width-cap placement headroom must avoid repeating expensive Nona map generation");

  const fs::path rounded_canvas_game = root / "nona-rounded-canvas-game";
  fs::create_directories(rounded_canvas_game);
  hm::stitching::HuginProject::Options rounded_canvas_options = options;
  rounded_canvas_options.max_canvas_dimension.reset();
  rounded_canvas_options.max_output_width = 80;
  rounded_canvas_options.progress = {};
  ::setenv("HM_TEST_PANO_ROUND_WIDTH", "1", 1);
  const auto rounded_canvas_configured = hm::stitching::HuginProject::Configure(
      rounded_canvas_game,
      root / "private-inputs" / "left.png",
      root / "private-inputs" / "right.png",
      matches,
      rounded_canvas_options);
  ::unsetenv("HM_TEST_PANO_ROUND_WIDTH");
  ok &= expect(
      rounded_canvas_configured.ok(),
      "pano_modify may round a guarded odd canvas request upward by one pixel without invalidating calibration");

  const fs::path retry_native_fixtures = root / "retry-native-fixtures";
  const fs::path retry_capped_fixtures = root / "retry-capped-fixtures";
  fs::create_directories(retry_native_fixtures);
  fs::create_directories(retry_capped_fixtures);
  ok &= expect(
      write_spatial_tiff_tags(retry_native_fixtures / "mapping_0000.tif", 50, 32, 0.0f, 1.0f) &&
          write_spatial_tiff_tags(retry_native_fixtures / "mapping_0001.tif", 51, 32, 50.0f, 1.0f) &&
          write_remap_pair(retry_native_fixtures, "mapping_0000", 50, 32) &&
          write_remap_pair(retry_native_fixtures, "mapping_0001", 51, 32) &&
          write_spatial_tiff_tags(retry_capped_fixtures / "mapping_0000.tif", 50, 32, 0.0f, 1.0f) &&
          write_spatial_tiff_tags(retry_capped_fixtures / "mapping_0001.tif", 50, 32, 50.0f, 1.0f) &&
          write_remap_pair(retry_capped_fixtures, "mapping_0000", 50, 32) &&
          write_remap_pair(retry_capped_fixtures, "mapping_0001", 50, 32),
      "Nona retry fixtures must describe 101-pixel native and 100-pixel capped canvases");
  cv::Mat retry_seam(32, 100, CV_8U, cv::Scalar(0));
  retry_seam.colRange(50, retry_seam.cols).setTo(255);
  ok &= expect(
      cv::imwrite((retry_capped_fixtures / "seam_file.png").string(), retry_seam) &&
          cv::imwrite(
              (retry_capped_fixtures / "panorama.tif").string(), cv::Mat(32, 100, CV_8UC3, cv::Scalar(1, 2, 3))),
      "Nona retry preview fixtures must match the capped canvas");
  ok &= expect(
      write_tool(
          nona,
          "fixtures='" + retry_capped_fixtures.string() +
              "'\n"
              "if grep -q ' w100 ' autooptimiser_out.pto; then fixtures='" +
              retry_native_fixtures.string() +
              "'; fi\n"
              "for file in mapping_0000.tif mapping_0000_x.tif mapping_0000_y.tif mapping_0001.tif "
              "mapping_0001_x.tif mapping_0001_y.tif; do cp \"$fixtures/$file\" \"$file\"; done\n") &&
          write_tool(
              enblend,
              "cp '" + retry_capped_fixtures.string() +
                  "/seam_file.png' seam_file.png\n"
                  "cp '" +
                  retry_capped_fixtures.string() + "/panorama.tif' panorama.tif\n"),
      "Nona retry tools must switch from measured native output to capped output");
  const fs::path retry_game = root / "nona-retry-game";
  fs::create_directories(retry_game);
  hm::stitching::HuginProject::Options retry_options = options;
  retry_options.max_canvas_dimension.reset();
  retry_options.max_output_width = 100;
  retry_options.progress = {};
  const auto retry_configured = hm::stitching::HuginProject::Configure(
      retry_game, root / "private-inputs" / "left.png", root / "private-inputs" / "right.png", matches, retry_options);
  ok &= expect(retry_configured.ok(), "Nona placement-only width overflow must succeed after a constrained retry");
  auto retry_lock = hm::stitching::HuginProject::RecoverAndLock(retry_game);
  ok &= expect(retry_lock.ok(), "Nona retry provenance must be readable");
  if (retry_lock.ok()) {
    const auto retry_provenance = hm::stitching::HuginProject::ReadCanvasProvenance(retry_game, **retry_lock);
    ok &= expect(
        retry_provenance.ok() && retry_provenance->has_value() && (*retry_provenance)->source_canvas_width == 101 &&
            (*retry_provenance)->source_canvas_height == 50 && (*retry_provenance)->canvas_width == 100 &&
            (*retry_provenance)->canvas_height == 32 && (*retry_provenance)->max_output_width_applied,
        "Nona retry provenance must record the measured unconstrained remap canvas, not only the PTO canvas");
    retry_lock->reset();
  }
  ok &= expect(
      write_tool(
          nona,
          "for file in mapping_0000.tif mapping_0000_x.tif mapping_0000_y.tif mapping_0001.tif "
          "mapping_0001_x.tif mapping_0001_y.tif; do cp '" +
              fixtures.string() + "/'$file \"$file\"; done\n") &&
          write_tool(
              enblend,
              "cp '" + fixtures.string() +
                  "/seam_file.png' seam_file.png\n"
                  "cp '" +
                  fixtures.string() + "/panorama.tif' panorama.tif\n"),
      "working fake Nona and enblend tools must be restored after retry provenance validation");

  const auto optimizer_args_size = fs::file_size(autooptimiser_args);
  hm::stitching::HuginProject::Options optimizer_disabled_options;
  std::string optimizer_disabled_message;
  fs::create_directories(root / "optimizer-disabled-game");
  std::vector<hm::stitching::FeatureMatch> optimizer_disabled_matches;
  for (int y = 6; y < 48; y += 10) {
    for (int x = 16; x < 64; x += 10) {
      optimizer_disabled_matches.push_back(
          {{static_cast<float>(x), static_cast<float>(y)},
           {static_cast<float>(x - 8), static_cast<float>(y + 3)},
           0.9f});
    }
  }
  cv::Mat optimizer_disabled_seam(51, 73, CV_8UC1, cv::Scalar(0));
  optimizer_disabled_seam.colRange(36, optimizer_disabled_seam.cols).setTo(cv::Scalar(255));
  ok &= expect(
      cv::imwrite((root / "optimizer-disabled-seam.png").string(), optimizer_disabled_seam),
      "optimizer-disabled seam fixture must exist");
  ok &= expect(
      write_tool(
          enblend,
          "cp '" + (root / "optimizer-disabled-seam.png").string() +
              "' seam_file.png\n"
              "cp '" +
              fixtures.string() + "/panorama.tif' panorama.tif\n"),
      "optimizer-disabled fake enblend must preserve the native seam");
  optimizer_disabled_options.progress =
      [&](const std::string& stage, const std::string& status, const std::string& message) {
        if (stage == "optimizer" && status == "complete")
          optimizer_disabled_message = message;
      };
  ::setenv("HM_AUTOOPTIMISER", (root / "missing-autooptimiser-disabled-by-default").c_str(), 1);
  const auto optimizer_disabled = hm::stitching::HuginProject::Configure(
      root / "optimizer-disabled-game",
      root / "private-inputs" / "left.png",
      root / "private-inputs" / "right.png",
      optimizer_disabled_matches,
      optimizer_disabled_options);
  ::setenv("HM_AUTOOPTIMISER", autooptimiser.c_str(), 1);
  if (!optimizer_disabled.ok())
    std::cerr << optimizer_disabled << '\n';
  ok &= expect(optimizer_disabled.ok(), "Hugin calibration must succeed with its default optimizer setting disabled");
  ok &= expect(
      fs::file_size(autooptimiser_args) == optimizer_args_size,
      "Hugin calibration must skip autooptimiser unless explicitly enabled");
  ok &= expect(
      optimizer_disabled_message.find("disabled") != std::string::npos,
      "Optimizer-disabled calibration progress must explain that the stage was skipped");
  if (optimizer_disabled.ok()) {
    std::ifstream generated(root / "optimizer-disabled-game" / "hm_project.pto", std::ios::binary);
    std::ifstream published(root / "optimizer-disabled-game" / "autooptimiser_out.pto", std::ios::binary);
    const std::string generated_project((std::istreambuf_iterator<char>(generated)), std::istreambuf_iterator<char>());
    const std::string published_project((std::istreambuf_iterator<char>(published)), std::istreambuf_iterator<char>());
    ok &= expect(
        generated_project == published_project,
        "optimizer-disabled calibration must publish the generated Hugin project without modifying its geometry");
  }

  hm::stitching::HuginProject::Options six_point_akaze_options;
  six_point_akaze_options.control_point_matcher = hm::stitching::ControlPointMatcher::kAkazeHamming;
  std::vector<hm::stitching::FeatureMatch> six_point_akaze_matches;
  for (const int y : {6, 36}) {
    for (const int x : {16, 36, 56}) {
      six_point_akaze_matches.push_back(
          {{static_cast<float>(x), static_cast<float>(y)},
           {static_cast<float>(x - 8), static_cast<float>(y + 3)},
           0.9f});
    }
  }
  fs::create_directories(root / "six-point-akaze-game");
  ::setenv("HM_ALLOW_HARD_SEAM_FALLBACK", "1", 1);
  const auto six_point_akaze = hm::stitching::HuginProject::Configure(
      root / "six-point-akaze-game",
      root / "private-inputs" / "left.png",
      root / "private-inputs" / "right.png",
      six_point_akaze_matches,
      six_point_akaze_options);
  ::unsetenv("HM_ALLOW_HARD_SEAM_FALLBACK");
  if (!six_point_akaze.ok())
    std::cerr << six_point_akaze << '\n';
  ok &= expect(six_point_akaze.ok(), "native AKAZE mapping must honor its six-control-point minimum");

  hm::stitching::HuginProject::Options invalid_nona_options;
  invalid_nona_options.mapping_backend = hm::stitching::MappingBackend::kNona;
  const auto invalid_nona = hm::stitching::HuginProject::Configure(
      root / "optimizer-disabled-game",
      root / "private-inputs" / "left.png",
      root / "private-inputs" / "right.png",
      matches,
      invalid_nona_options);
  ok &= expect(
      absl::IsInvalidArgument(invalid_nona) &&
          std::string(invalid_nona.message()).find("requires stitching.run_autooptimizer=true") != std::string::npos,
      "NONA must reject optimizer-disabled calibration instead of publishing unaligned camera geometry");

  hm::stitching::HuginProject::Options calibrated_nona_options;
  calibrated_nona_options.mapping_backend = hm::stitching::MappingBackend::kNona;
  calibrated_nona_options.run_autooptimizer = true;
  hm::stitching::FisheyeLensCalibration nona_lens;
  nona_lens.resolution = {64, 48};
  nona_lens.fx = 40.0;
  nona_lens.fy = 40.0;
  nona_lens.cx = 32.0;
  nona_lens.cy = 24.0;
  calibrated_nona_options.akaze_calibration = {.left = nona_lens, .right = nona_lens};
  const auto calibrated_nona = hm::stitching::HuginProject::Configure(
      root / "calibrated-nona-game",
      root / "private-inputs" / "left.png",
      root / "private-inputs" / "right.png",
      matches,
      calibrated_nona_options);
  ok &= expect(
      absl::IsInvalidArgument(calibrated_nona) &&
          std::string(calibrated_nona.message()).find("does not consume the GoPro KB4 lens profile") !=
              std::string::npos,
      "NONA must reject rectified calibrated AKAZE control points");

  const fs::path fallback_game = root / "fallback-game";
  fs::create_directories(fallback_game);
  ok &= expect(write_tool(enblend, "exit 44\n"), "failing fake enblend must be created");
  ::unsetenv("HM_ALLOW_HARD_SEAM_FALLBACK");
  const auto fallback_disabled = hm::stitching::HuginProject::Configure(
      fallback_game, root / "private-inputs" / "left.png", root / "private-inputs" / "right.png", matches, options);
  ok &= expect(
      absl::IsFailedPrecondition(fallback_disabled) &&
          std::string(fallback_disabled.message()).find("HM_ALLOW_HARD_SEAM_FALLBACK=1") != std::string::npos &&
          !fs::exists(fallback_game / "seam_file.png"),
      "failed enblend must fail closed without publishing a hard seam");
  ::setenv("HM_ALLOW_HARD_SEAM_FALLBACK", "1", 1);
  const auto fallback_enabled = hm::stitching::HuginProject::Configure(
      fallback_game, root / "private-inputs" / "left.png", root / "private-inputs" / "right.png", matches, options);
  ::unsetenv("HM_ALLOW_HARD_SEAM_FALLBACK");
  ok &= expect(
      fallback_enabled.ok() && fs::is_regular_file(fallback_game / "seam_file.png"),
      "HM_ALLOW_HARD_SEAM_FALLBACK=1 must permit transactional hard-seam generation");
  ok &= expect(
      write_tool(
          enblend,
          "cp '" + fixtures.string() +
              "/seam_file.png' seam_file.png\n"
              "cp '" +
              fixtures.string() + "/panorama.tif' panorama.tif\n"),
      "working fake enblend must be restored");

  const fs::path opencv_game = root / "opencv-game";
  const fs::path opencv_enblend_args = root / "opencv-enblend.args";
  fs::create_directories(opencv_game);
  ok &= expect(
      write_tool(
          enblend,
          "printf '%s\\n' \"$*\" > '" + opencv_enblend_args.string() +
              "'\n"
              "test -f seam_file.png\n"
              "cp '" +
              fixtures.string() + "/panorama.tif' panorama.tif\n"),
      "OpenCV fake enblend must preserve the native seam");
  ::setenv("HM_AUTOOPTIMISER", (root / "missing-autooptimiser-for-opencv").c_str(), 1);
  hm::stitching::HuginProject::Options opencv_options;
  opencv_options.mapping_backend = hm::stitching::MappingBackend::kOpenCvAffineRansac;
  opencv_options.max_canvas_dimension = 96;
  std::vector<hm::stitching::FeatureMatch> opencv_matches;
  for (int y = 6; y < 48; y += 10) {
    for (int x = 16; x < 64; x += 10) {
      opencv_matches.push_back(
          {{static_cast<float>(x), static_cast<float>(y)},
           {static_cast<float>(x - 8), static_cast<float>(y + 3)},
           0.9f});
    }
  }
  const auto opencv_configured = hm::stitching::HuginProject::Configure(
      opencv_game,
      root / "private-inputs" / "left.png",
      root / "private-inputs" / "right.png",
      opencv_matches,
      opencv_options);
  if (!opencv_configured.ok())
    std::cerr << opencv_configured << '\n';
  ok &= expect(opencv_configured.ok(), "OpenCV mapping backend must not require autooptimiser");
  ok &= expect(
      fs::is_regular_file(opencv_game / "autooptimiser_out.pto") &&
          fs::is_regular_file(opencv_game / "mapping_0000.tif") &&
          fs::is_regular_file(opencv_game / "mapping_0001.tif") && fs::is_regular_file(opencv_game / "seam_file.png"),
      "OpenCV mapping backend must publish project, maps, and seam artifacts");
  {
    std::ifstream input(opencv_game / "autooptimiser_out.pto", std::ios::binary);
    const std::string copied_project((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ok &= expect(
        copied_project.find("# control points") != std::string::npos,
        "OpenCV mapping backend must publish the control-point PTO as autooptimiser_out.pto");
  }
  {
    std::ifstream input(opencv_enblend_args, std::ios::binary);
    const std::string opencv_enblend((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ok &= expect(
        opencv_enblend.find("mapping_0000.tif") != std::string::npos &&
            opencv_enblend.find("mapping_0001.tif") != std::string::npos,
        "OpenCV mapping backend must still run enblend over native mapping TIFFs");
  }
  ::setenv("HM_AUTOOPTIMISER", autooptimiser.c_str(), 1);
  ok &= expect(
      write_tool(
          enblend,
          "cp '" + fixtures.string() +
              "/seam_file.png' seam_file.png\n"
              "cp '" +
              fixtures.string() + "/panorama.tif' panorama.tif\n"),
      "working fake enblend must be restored after OpenCV backend test");

  const auto previous_project = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  {
    std::ofstream config(root / "game" / "config.yaml");
    config << "hstream_ui:\n"
              "  stitching_calibration:\n"
              "    status: pending\n"
              "    artifacts_invalidated: true\n"
              "    invalidation_id: hugin-run-a\n";
  }
  options.expected_invalidation_id = "hugin-run-a";
  bool superseded_during_hugin = false;
  options.progress = [&](const std::string& stage, const std::string& status, const std::string&) {
    if (stage != "canvas" || status != "started" || superseded_during_hugin)
      return;
    superseded_during_hugin = true;
    std::ofstream config(root / "game" / "config.yaml");
    config << "hstream_ui:\n"
              "  stitching_calibration:\n"
              "    status: pending\n"
              "    artifacts_invalidated: false\n"
              "    invalidation_id: hugin-run-b\n";
  };
  const auto superseded_hugin = hm::stitching::HuginProject::Configure(root / "game", matches, options);
  ok &= expect(
      superseded_during_hugin && superseded_hugin.code() == absl::StatusCode::kAborted,
      "superseded Hugin calibration must abort at its publication transaction");
  const auto project_after_superseded_hugin = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  const auto config_after_superseded_hugin = [&]() {
    std::ifstream input(root / "game" / "config.yaml", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(
      project_after_superseded_hugin == previous_project &&
          config_after_superseded_hugin.find("invalidation_id: hugin-run-b") != std::string::npos,
      "superseded Hugin publication must preserve both the prior artifacts and the newer invalidation");

  YAML::Node backend_claim(YAML::NodeType::Map);
  const hm::stitching::StitchingBackendChoices expected_backend_choices{
      "superpoint-lightglue",
      "nona",
      "equirectangular",
      true,
      {},
      hm::stitching::StitchProjectionFraming{true, 180.0, true, true}};
  backend_claim["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  backend_claim["stitching"]["mapping_backend"] = "nona";
  backend_claim["stitching"]["projection"] = "equirectangular";
  backend_claim["stitching"]["run_autooptimizer"] = true;
  hm::stitching::write_stitch_projection_framing(
      backend_claim, expected_backend_choices.projection_framing);
  backend_claim["hstream_ui"]["stitching_calibration"]["status"] = "pending";
  backend_claim["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = true;
  backend_claim["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "hugin-backend-a";
  backend_claim["hstream_ui"]["stitching_calibration"]["backend_generation"]["invalidation_id"] = "hugin-backend-a";
  backend_claim["hstream_ui"]["stitching_calibration"]["backend_generation"]["control_point_matcher"] =
      expected_backend_choices.control_point_matcher;
  backend_claim["hstream_ui"]["stitching_calibration"]["backend_generation"]["mapping_backend"] =
      expected_backend_choices.mapping_backend;
  backend_claim["hstream_ui"]["stitching_calibration"]["backend_generation"]["projection"] =
      expected_backend_choices.projection;
  backend_claim["hstream_ui"]["stitching_calibration"]["backend_generation"]["run_autooptimizer"] =
      expected_backend_choices.run_autooptimizer;
  backend_claim["hstream_ui"]["stitching_calibration"]["backend_generation"]["projection_parameters"] =
      YAML::Load("[]");
  YAML::Node backend_claim_framing =
      backend_claim["hstream_ui"]["stitching_calibration"]["backend_generation"]["projection_framing"];
  backend_claim_framing["auto_fov"] = expected_backend_choices.projection_framing.auto_fov;
  backend_claim_framing["horizontal_fov"] = expected_backend_choices.projection_framing.horizontal_fov;
  backend_claim_framing["auto_canvas"] = expected_backend_choices.projection_framing.auto_canvas;
  backend_claim_framing["auto_crop"] = expected_backend_choices.projection_framing.auto_crop;
  std::ofstream(root / "game" / "config.yaml") << YAML::Dump(backend_claim) << '\n';
  options.expected_invalidation_id = "hugin-backend-a";
  options.expected_backend_choices = expected_backend_choices;
  bool backend_changed_during_hugin = false;
  options.progress = [&](const std::string& stage, const std::string& status, const std::string&) {
    if (stage != "canvas" || status != "started" || backend_changed_during_hugin)
      return;
    backend_changed_during_hugin = true;
    YAML::Node changed = YAML::LoadFile((root / "game" / "config.yaml").string());
    changed["stitching"]["projection"] = "general-panini";
    std::ofstream(root / "game" / "config.yaml") << YAML::Dump(changed) << '\n';
  };
  const auto mismatched_backend_hugin = hm::stitching::HuginProject::Configure(root / "game", matches, options);
  const auto project_after_mismatched_backend = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(
      backend_changed_during_hugin && absl::IsAborted(mismatched_backend_hugin) &&
          project_after_mismatched_backend == previous_project,
      "Hugin publication must abort when the worker-visible projection changes under its generation claim");
  options.expected_invalidation_id.clear();
  options.expected_backend_choices.reset();
  {
    std::ofstream config(root / "game" / "config.yaml");
    config << "hstream_ui:\n"
              "  stitching_calibration:\n"
              "    status: complete\n"
              "    invalidation_id: hugin-run-a\n";
  }
  options.expected_invalidation_id = "hugin-run-a";
  options.progress = {};
  const auto completed_owner_hugin = hm::stitching::HuginProject::Configure(root / "game", matches, options);
  const auto project_after_completed_owner = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(
      completed_owner_hugin.code() == absl::StatusCode::kAborted && project_after_completed_owner == previous_project,
      "a Hugin producer must not replace completed calibration artifacts for the same invalidation owner");
  options.expected_invalidation_id.clear();
  const auto pending_live_owner = hm::stitching::current_live_stitched_output_owner_process();
  ok &= expect(pending_live_owner.ok(), "pending live authorization fixture must identify its owner process");
  {
    std::ofstream config(root / "game" / "config.yaml");
    config << "rink:\n"
              "  stitched_output_pending_generation: pending-live-generation\n"
              "  stitched_output_pending_authorization_id: pending-live-authorization\n"
              "  stitched_output_pending_owner_process: "
           << (pending_live_owner.ok() ? *pending_live_owner : std::string("invalid")) << '\n';
  }
  const auto pending_live_authorization = hm::stitching::HuginProject::Configure(root / "game", matches, options);
  const auto project_after_pending_live_authorization = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(
      pending_live_authorization.code() == absl::StatusCode::kAborted &&
          project_after_pending_live_authorization == previous_project,
      "pending live stitched-output authorization must prevent Hugin artifact replacement");
  std::ofstream(root / "game" / "config.yaml") << "unrelated: preserved\n";
  ok &= expect(
      fs::remove(root / "game" / "stitching_generation_id"),
      "legacy generation fixture must remove the logical identity sidecar");

  const fs::path symlinked_artifact = root / "game" / "panorama.tif";
  const fs::path symlink_target = root / "symlink-target.tif";
  std::error_code symlink_error;
  fs::copy_file(symlinked_artifact, symlink_target, fs::copy_options::overwrite_existing, symlink_error);
  const std::vector<unsigned char> symlink_target_contents = read_binary_file(symlink_target);
  fs::remove(symlinked_artifact, symlink_error);
  fs::create_symlink(symlink_target, symlinked_artifact, symlink_error);
  const auto symlinked_publication = hm::stitching::HuginProject::Configure(root / "game", matches, options);
  const bool symlink_rejected = !symlink_error && absl::IsFailedPrecondition(symlinked_publication) &&
      fs::is_symlink(fs::symlink_status(symlinked_artifact)) &&
      read_binary_file(symlink_target) == symlink_target_contents;
  if (!symlink_rejected)
    std::cerr << "symlinked prior-artifact publication: " << symlinked_publication << '\n';
  ok &=
      expect(symlink_rejected, "Hugin publication must reject a symlinked prior artifact without mutating its target");
  fs::remove(symlinked_artifact, symlink_error);
  fs::copy_file(symlink_target, symlinked_artifact, fs::copy_options::overwrite_existing, symlink_error);
  fs::remove(symlink_target, symlink_error);
  ok &= expect(!symlink_error, "symlinked prior-artifact fixture must restore the regular artifact");
  ok &= expect(
      fs::remove(root / "game" / "stitching_generation_id"),
      "sidecar-free legacy fixture must remove the identity sidecar recreated during rollback");
  std::string adopted_parent_generation;
  std::string adopted_parent_repeat;
  {
    auto adoption_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
    ok &= expect(adoption_lock.ok(), "sidecar-free generation fixture must lock");
    if (adoption_lock.ok()) {
      const auto first = hm::stitching::HuginProject::GenerationId(root / "game", **adoption_lock);
      const auto second = hm::stitching::HuginProject::GenerationId(root / "game", **adoption_lock);
      if (first.ok())
        adopted_parent_generation = *first;
      if (second.ok())
        adopted_parent_repeat = *second;
    }
  }
  const bool adopted_child_matches = generation_matches_in_child(root / "game", adopted_parent_generation);
  const bool adopted_generation_stable = !adopted_parent_generation.empty() &&
      adopted_parent_generation == adopted_parent_repeat && adopted_child_matches &&
      read_text_file(root / "game" / "stitching_generation_id").rfind("version=3\n", 0) == 0;
  if (!adopted_generation_stable) {
    std::cerr << "sidecar-free generation fixture: parent-bytes=" << adopted_parent_generation.size()
              << " repeat-bytes=" << adopted_parent_repeat.size() << " child-matches=" << adopted_child_matches
              << " sidecar=" << fs::exists(root / "game" / "stitching_generation_id") << '\n';
  }
  ok &= expect(
      adopted_generation_stable,
      "sidecar-free generations must be content-adopted once and remain stable across processes");

  const fs::path full_digest_artifact = root / "game" / "hm_project.pto";
  const std::vector<unsigned char> original_full_digest_artifact = read_binary_file(full_digest_artifact);
  {
    std::ofstream output(full_digest_artifact, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(original_full_digest_artifact.data()),
        static_cast<std::streamsize>(original_full_digest_artifact.size()));
    const std::string padding(1024 * 1024, 'x');
    output.write(padding.data(), static_cast<std::streamsize>(padding.size()));
  }
  ::setenv("HM_TEST_STITCH_UNRELIABLE_METADATA", "1", 1);
  auto full_digest_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
  const auto full_digest_before = full_digest_lock.ok()
      ? hm::stitching::HuginProject::GenerationId(root / "game", **full_digest_lock)
      : absl::StatusOr<std::string>(full_digest_lock.status());
  if (full_digest_lock.ok())
    full_digest_lock->reset();
  {
    std::fstream output(full_digest_artifact, std::ios::binary | std::ios::in | std::ios::out);
    output.seekp(400 * 1024);
    output.put('y');
  }
  full_digest_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
  const auto full_digest_after = full_digest_lock.ok()
      ? hm::stitching::HuginProject::GenerationId(root / "game", **full_digest_lock)
      : absl::StatusOr<std::string>(full_digest_lock.status());
  if (full_digest_lock.ok())
    full_digest_lock->reset();
  ::unsetenv("HM_TEST_STITCH_UNRELIABLE_METADATA");
  ok &= expect(
      full_digest_before.ok() && full_digest_after.ok() && *full_digest_before != *full_digest_after,
      "weak-filesystem generations must detect same-size changes outside the former sampled regions");
  {
    std::ofstream output(full_digest_artifact, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(original_full_digest_artifact.data()),
        static_cast<std::streamsize>(original_full_digest_artifact.size()));
  }

  std::string generation_before_interrupted_publication;
  {
    auto generation_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
    ok &= expect(generation_lock.ok(), "Hugin generation fixture must lock before interrupted publication");
    if (generation_lock.ok()) {
      auto generation = hm::stitching::HuginProject::GenerationId(root / "game", **generation_lock);
      ok &= expect(generation.ok(), "Hugin generation fixture must be identifiable before interrupted publication");
      if (generation.ok())
        generation_before_interrupted_publication = *generation;
    }
  }
  ::setenv("HM_TEST_STITCH_INTERRUPT_AFTER_PREPARE_SYNC", "1", 1);
  const auto interrupted_before_publication = hm::stitching::HuginProject::Configure(root / "game", matches, options);
  ::unsetenv("HM_TEST_STITCH_INTERRUPT_AFTER_PREPARE_SYNC");
  ok &= expect(
      !interrupted_before_publication.ok(), "injected interruption after durable preparation must stop publication");
  bool durable_prepared_journal = false;
  for (const auto& entry : fs::directory_iterator(root / "game")) {
    if (entry.is_directory() && entry.path().filename().string().rfind(".hstream-stitch-", 0) == 0)
      durable_prepared_journal = true;
  }
  ok &= expect(durable_prepared_journal, "durably prepared Hugin publication must retain its recovery journal");
  const auto project_before_recovery = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(
      project_before_recovery == previous_project,
      "interruption after durable preparation must happen before replacing root artifacts");
  ok &= expect(
      hm::stitching::HuginProject::Recover(root / "game").ok(),
      "durably prepared Hugin publication must recover on the next owner");
  const auto project_after_recovery = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(project_after_recovery == previous_project, "prepared-only Hugin recovery must preserve the generation");
  {
    auto generation_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
    ok &= expect(generation_lock.ok(), "recovered Hugin generation must remain lockable");
    if (generation_lock.ok()) {
      auto generation = hm::stitching::HuginProject::GenerationId(root / "game", **generation_lock);
      ok &= expect(
          generation.ok() && *generation == generation_before_interrupted_publication,
          "Hugin rollback must preserve the logical generation identity");
    }
  }
  ::setenv("HM_TEST_STITCH_INTERRUPT_DURING_BACKUP", "1", 1);
  const auto interrupted_during_backup = hm::stitching::HuginProject::Configure(root / "game", matches, options);
  ::unsetenv("HM_TEST_STITCH_INTERRUPT_DURING_BACKUP");
  ok &= expect(!interrupted_during_backup.ok(), "injected interruption during artifact backup must stop publication");
  ok &= expect(
      fs::exists(root / "game" / "hm_project.pto") && fs::exists(root / "game" / "autooptimiser_out.pto"),
      "backup-in-progress interruption must preserve every root artifact");
  bool durable_partial_backup = false;
  for (const auto& entry : fs::directory_iterator(root / "game")) {
    const fs::path backup = entry.path() / "previous" / "hm_project.pto";
    if (entry.is_directory() && entry.path().filename().string().rfind(".hstream-stitch-", 0) == 0 &&
        fs::is_regular_file(backup)) {
      fs::remove(backup);
      std::ofstream(backup) << "partial\n";
      std::ofstream(backup.parent_path() / ".mapping_0000.tif.hstream-partial") << "partial\n";
      durable_partial_backup = true;
      break;
    }
  }
  ok &= expect(
      durable_partial_backup, "backup-in-progress recovery fixture must retain its first durable private backup");
  const auto partial_backup_recovery = hm::stitching::HuginProject::Recover(root / "game");
  ok &= expect(
      partial_backup_recovery.ok(),
      "backup-in-progress recovery must discard partial backups and keep a complete unchanged root generation");
  {
    auto generation_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
    ok &= expect(generation_lock.ok(), "partially backed-up Hugin generation must remain lockable");
    if (generation_lock.ok()) {
      auto generation = hm::stitching::HuginProject::GenerationId(root / "game", **generation_lock);
      ok &= expect(
          generation.ok() && *generation == generation_before_interrupted_publication,
          "partial backup rollback must preserve the logical generation identity");
    }
  }
  ::setenv("HM_TEST_STITCH_INTERRUPT_AFTER_BACKUP_SYNC", "1", 1);
  const auto interrupted_after_backup = hm::stitching::HuginProject::Configure(root / "game", matches, options);
  ::unsetenv("HM_TEST_STITCH_INTERRUPT_AFTER_BACKUP_SYNC");
  ok &= expect(!interrupted_after_backup.ok(), "injected interruption after durable backup must stop publication");
  ok &= expect(
      fs::exists(root / "game" / "hm_project.pto") && fs::exists(root / "game" / "autooptimiser_out.pto"),
      "durable backup completion must not remove root artifacts before replacement publication");
  fs::path rollback_transaction;
  for (const auto& entry : fs::directory_iterator(root / "game")) {
    if (entry.is_directory() && entry.path().filename().string().rfind(".hstream-stitch-", 0) == 0) {
      rollback_transaction = entry.path();
      break;
    }
  }
  ok &= expect(!rollback_transaction.empty(), "backed-up interruption must retain its transaction journal");
  const auto journal_version_time = rollback_transaction.empty()
      ? fs::file_time_type::min()
      : fs::last_write_time(rollback_transaction / "journal_version");
  ::setenv("HM_TEST_STITCH_ROLLBACK_INTERRUPT_AFTER", "1", 1);
  ::setenv("HM_TEST_STITCH_DISABLE_LINK_CLONE", "1", 1);
  const auto interrupted_rollback = hm::stitching::HuginProject::Recover(root / "game");
  ::unsetenv("HM_TEST_STITCH_ROLLBACK_INTERRUPT_AFTER");
  ok &= expect(!interrupted_rollback.ok(), "interrupted Hugin rollback must retain a resumable journal");
  ::setenv("HM_TEST_STITCH_INTERRUPT_AFTER_RESTORED_SYNC", "1", 1);
  const auto interrupted_restored_cleanup = hm::stitching::HuginProject::Recover(root / "game");
  ::unsetenv("HM_TEST_STITCH_INTERRUPT_AFTER_RESTORED_SYNC");
  ok &= expect(
      !interrupted_restored_cleanup.ok(), "restored Hugin generation must retain its journal before backup cleanup");
  ok &= expect(
      !rollback_transaction.empty() &&
          fs::last_write_time(rollback_transaction / "journal_version") == journal_version_time,
      "rollback recovery must not rewrite an already-durable journal version");
  const fs::path rollback_state_target = root / "hugin-rollback-state-target";
  std::ofstream(rollback_state_target) << "preserved\n";
  if (!rollback_transaction.empty())
    fs::create_symlink(rollback_state_target, rollback_transaction / "state.rolled_back");
  ok &= expect(
      hm::stitching::HuginProject::Recover(root / "game").ok(),
      "durably backed-up Hugin publication must recover after interrupted restore and cleanup");
  ok &= expect(
      read_text_file(rollback_state_target) == "preserved\n",
      "Hugin rollback state publication must replace a journal symlink without following it");
  fs::remove(rollback_state_target);
  ::unsetenv("HM_TEST_STITCH_DISABLE_LINK_CLONE");
  {
    auto generation_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
    ok &= expect(generation_lock.ok(), "fully backed-up Hugin generation must remain lockable");
    if (generation_lock.ok()) {
      auto generation = hm::stitching::HuginProject::GenerationId(root / "game", **generation_lock);
      ok &= expect(
          generation.ok() && *generation == generation_before_interrupted_publication,
          "complete backup rollback must preserve the logical generation identity");
    }
  }
  ok &= expect(
      write_remap_pair(fixtures, "mapping_0000", 40, 32, true) &&
          write_remap_pair(fixtures, "mapping_0001", 40, 32, true),
      "degenerate remap fixtures must exist");
  const fs::path symlinked_game = root / "symlinked-game";
  fs::create_directory_symlink(root / "game", symlinked_game);
  ok &= expect(
      hm::stitching::HuginProject::Recover(symlinked_game).ok(),
      "Hugin recovery must follow and pin a caller-selected symlinked game directory");
  fs::remove(symlinked_game);
  const auto degenerate = hm::stitching::HuginProject::Configure(root / "game", matches, options);
  ok &= expect(!degenerate.ok(), "constant Hugin remaps must be rejected before publication");
  ok &= expect(
      write_remap_pair(fixtures, "mapping_0000", 40, 32) && write_remap_pair(fixtures, "mapping_0001", 40, 32),
      "valid remap fixtures must be restored");
  const auto after_degenerate = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(previous_project == after_degenerate, "degenerate remaps must preserve the prior Hugin generation");
  ok &= expect(
      write_tool(
          nona,
          "for file in mapping_0000.tif mapping_0000_x.tif mapping_0000_y.tif mapping_0001.tif "
          "mapping_0001_x.tif mapping_0001_y.tif; do printf x > \"$file\"; done\n"),
      "invalid fake nona must be created");
  const auto invalid = hm::stitching::HuginProject::Configure(root / "game", matches, options);
  ok &= expect(!invalid.ok(), "undecodable Hugin remaps must be rejected before publication");
  const auto preserved_project = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(previous_project == preserved_project, "failed Hugin validation must preserve the prior generation");
  ok &= expect(
      fs::file_size(root / "game" / "mapping_0000.tif") > 1, "failed Hugin validation must not publish a corrupt TIFF");

  const std::vector<std::string> artifact_names = {
      "left.png",
      "right.png",
      "hm_project.pto",
      "autooptimiser_out.pto",
      "mapping_0000.tif",
      "mapping_0000_x.tif",
      "mapping_0000_y.tif",
      "mapping_0001.tif",
      "mapping_0001_x.tif",
      "mapping_0001_y.tif",
      "seam_file.png",
      "panorama.tif",
      "stitching_canvas_provenance",
  };
  const auto write_legacy_transaction_fixture = [&](const fs::path& game, const fs::path& transaction) {
    fs::create_directories(transaction / "previous");
    std::ofstream manifest(transaction / "artifacts");
    for (const std::string& name : artifact_names) {
      manifest << name << '\n';
      std::ofstream(game / name) << "old-" << name << '\n';
      std::ofstream(transaction / "previous" / name) << "old-" << name << '\n';
    }
  };

  const fs::path ambiguous_root = root / "legacy-complete-unversioned";
  const fs::path ambiguous_transaction = ambiguous_root / ".hstream-stitch-ambiguous";
  fs::create_directories(ambiguous_root);
  write_legacy_transaction_fixture(ambiguous_root, ambiguous_transaction);
  {
    std::ofstream prior(ambiguous_transaction / "previous_artifacts");
    for (const std::string& name : artifact_names)
      prior << name << '\n';
  }
  std::ofstream(ambiguous_root / "mapping_0000.tif", std::ios::trunc) << "replacement\n";
  for (const char* state : {"PREPARED\n", "LEGACY_MIGRATE\n", "ROLLING_BACK\n"}) {
    std::ofstream(ambiguous_transaction / "state", std::ios::trunc) << state;
    const auto recovery = hm::stitching::HuginProject::Recover(ambiguous_root);
    ok &= expect(
        !recovery.ok() && fs::exists(ambiguous_transaction) &&
            read_text_file(ambiguous_root / "mapping_0000.tif") == "replacement\n",
        "an unversioned active journal must fail closed without restoring ambiguous backups");
  }
  fs::remove_all(ambiguous_root);

  const fs::path historical_backing_root = root / "historical-partial-backing";
  const fs::path historical_backing_transaction = historical_backing_root / ".hstream-stitch-backing";
  fs::create_directories(historical_backing_root);
  write_legacy_transaction_fixture(historical_backing_root, historical_backing_transaction);
  {
    std::ofstream prior(historical_backing_transaction / "previous_artifacts");
    for (const std::string& name : artifact_names)
      prior << name << '\n';
  }
  fs::remove(historical_backing_root / "hm_project.pto");
  fs::remove(historical_backing_transaction / "previous" / "mapping_0000.tif");
  std::ofstream(historical_backing_transaction / "previous" / "mapping_0000.tif") << "partial\n";
  std::ofstream(historical_backing_transaction / "previous" / ".mapping_0001.tif.hstream-partial") << "partial\n";
  std::ofstream(historical_backing_transaction / "journal_version") << "2\n";
  std::ofstream(historical_backing_transaction / "state") << "BACKING_UP\n";
  ::setenv("HM_TEST_STITCH_DISABLE_LINK_CLONE", "1", 1);
  const auto historical_backing_recovery = hm::stitching::HuginProject::Recover(historical_backing_root);
  ::unsetenv("HM_TEST_STITCH_DISABLE_LINK_CLONE");
  if (!historical_backing_recovery.ok())
    std::cerr << "historical BACKING_UP recovery: " << historical_backing_recovery << '\n';
  ok &= expect(
      historical_backing_recovery.ok() && !fs::exists(historical_backing_transaction) &&
          read_text_file(historical_backing_root / "hm_project.pto") == "old-hm_project.pto\n" &&
          read_text_file(historical_backing_root / "mapping_0000.tif") == "old-mapping_0000.tif\n",
      "historical BACKING_UP recovery must recreate suspect backups before restoring a missing root artifact");
  fs::remove_all(historical_backing_root);

  const fs::path oversized_restore_root = root / "oversized-restore-backup";
  const fs::path oversized_restore_transaction = oversized_restore_root / ".hstream-stitch-oversized-restore";
  fs::create_directories(oversized_restore_root);
  write_legacy_transaction_fixture(oversized_restore_root, oversized_restore_transaction);
  {
    std::ofstream prior(oversized_restore_transaction / "previous_artifacts");
    for (const std::string& name : artifact_names)
      prior << name << '\n';
  }
  std::ofstream(oversized_restore_root / "mapping_0000.tif", std::ios::trunc) << "replacement\n";
  const bool oversized_restore_created =
      ::truncate(
          (oversized_restore_transaction / "previous" / "mapping_0000.tif").c_str(), 1024LL * 1024LL * 1024LL + 1) == 0;
  std::ofstream(oversized_restore_transaction / "journal_version") << "2\n";
  std::ofstream(oversized_restore_transaction / "state") << "BACKED_UP\n";
  const auto oversized_restore_recovery = hm::stitching::HuginProject::Recover(oversized_restore_root);
  ok &= expect(
      oversized_restore_created && !oversized_restore_recovery.ok() && fs::exists(oversized_restore_transaction) &&
          read_text_file(oversized_restore_root / "mapping_0000.tif") == "replacement\n",
      "stitch recovery must reject an oversized backup before mutating replacement root artifacts");
  fs::remove_all(oversized_restore_root);

  const fs::path symlinked_transaction_root = root / "symlinked-transaction-root";
  const fs::path stitch_transaction_target = root / "stitch-transaction-symlink-target";
  const fs::path symlinked_stitch_transaction = symlinked_transaction_root / ".hstream-stitch-external";
  fs::create_directories(symlinked_transaction_root);
  fs::create_directories(stitch_transaction_target / "previous");
  std::ofstream(stitch_transaction_target / "previous" / "sentinel") << "preserved\n";
  std::ofstream(stitch_transaction_target / "journal_version") << "2\n";
  std::ofstream(stitch_transaction_target / "state") << "RESTORED\n";
  fs::create_directory_symlink(stitch_transaction_target, symlinked_stitch_transaction);
  const auto symlinked_transaction_recovery = hm::stitching::HuginProject::Recover(symlinked_transaction_root);
  ok &= expect(
      absl::IsFailedPrecondition(symlinked_transaction_recovery) &&
          fs::is_symlink(fs::symlink_status(symlinked_stitch_transaction)) &&
          read_text_file(stitch_transaction_target / "state") == "RESTORED\n" &&
          read_text_file(stitch_transaction_target / "previous" / "sentinel") == "preserved\n",
      "Hugin recovery must reject a symlinked transaction directory without mutating its external target");
  fs::remove_all(symlinked_transaction_root);
  fs::remove_all(stitch_transaction_target);

  const fs::path fifo_transaction_root = root / "fifo-transaction-root";
  const fs::path fifo_transaction = fifo_transaction_root / ".hstream-stitch-fifo";
  fs::create_directories(fifo_transaction);
  ok &= expect(::mkfifo((fifo_transaction / "state").c_str(), 0600) == 0, "FIFO stitch-state fixture must be created");
  const auto fifo_transaction_recovery = hm::stitching::HuginProject::Recover(fifo_transaction_root);
  ok &= expect(
      absl::IsFailedPrecondition(fifo_transaction_recovery) && fs::exists(fifo_transaction),
      "Hugin recovery must reject a FIFO state without blocking");
  fs::remove_all(fifo_transaction_root);

  const fs::path symlink_rollback_root = root / "symlinked-rollback-source";
  const fs::path symlink_rollback_transaction = symlink_rollback_root / ".hstream-stitch-backing";
  const fs::path rollback_symlink_target = root / "rollback-symlink-target.tif";
  fs::create_directories(symlink_rollback_root);
  write_legacy_transaction_fixture(symlink_rollback_root, symlink_rollback_transaction);
  {
    std::ofstream prior(symlink_rollback_transaction / "previous_artifacts");
    for (const std::string& name : artifact_names)
      prior << name << '\n';
  }
  fs::remove(symlink_rollback_root / "hm_project.pto");
  std::ofstream(rollback_symlink_target) << "outside-target\n";
  std::error_code rollback_symlink_error;
  fs::remove(symlink_rollback_root / "mapping_0000.tif", rollback_symlink_error);
  fs::create_symlink(rollback_symlink_target, symlink_rollback_root / "mapping_0000.tif", rollback_symlink_error);
  std::ofstream(symlink_rollback_transaction / "journal_version") << "2\n";
  std::ofstream(symlink_rollback_transaction / "state") << "BACKING_UP\n";
  const auto symlink_rollback_recovery = hm::stitching::HuginProject::Recover(symlink_rollback_root);
  ok &= expect(
      !rollback_symlink_error && absl::IsFailedPrecondition(symlink_rollback_recovery) &&
          fs::exists(symlink_rollback_transaction) &&
          fs::is_symlink(fs::symlink_status(symlink_rollback_root / "mapping_0000.tif")) &&
          read_text_file(rollback_symlink_target) == "outside-target\n",
      "rollback recovery must reject a symlinked root artifact without following or backing up its target");
  fs::remove_all(symlink_rollback_root);
  fs::remove(rollback_symlink_target);

  const fs::path consumed_root = root / "legacy-consumed-rollback";
  const fs::path consumed_transaction = consumed_root / ".hstream-stitch-consumed";
  fs::create_directories(consumed_root);
  write_legacy_transaction_fixture(consumed_root, consumed_transaction);
  fs::remove(consumed_transaction / "previous" / "hm_project.pto");
  std::ofstream(consumed_transaction / "state") << "PREPARED\n";
  const auto consumed_recovery = hm::stitching::HuginProject::Recover(consumed_root);
  ok &= expect(
      !consumed_recovery.ok() && fs::exists(consumed_transaction) &&
          read_text_file(consumed_root / "hm_project.pto") == "old-hm_project.pto\n",
      "legacy PREPARED recovery must fail closed when a backup may already have been consumed");
  fs::remove_all(consumed_root);

  const fs::path root_inclusive = root / "legacy-root-inclusive-manifest";
  const fs::path root_inclusive_transaction = root_inclusive / ".hstream-stitch-root-inclusive";
  fs::create_directories(root_inclusive);
  write_legacy_transaction_fixture(root_inclusive, root_inclusive_transaction);
  fs::remove(root_inclusive_transaction / "previous" / "stitching_canvas_provenance");
  std::ofstream(root_inclusive / "stitching_canvas_provenance", std::ios::trunc) << "replacement\n";
  {
    std::ofstream prior(root_inclusive_transaction / "previous_artifacts");
    for (const std::string& name : artifact_names)
      prior << name << '\n';
    std::ofstream(root_inclusive_transaction / "state") << "PREPARED\n";
  }
  const auto root_inclusive_recovery = hm::stitching::HuginProject::Recover(root_inclusive);
  ok &= expect(
      !root_inclusive_recovery.ok() && fs::exists(root_inclusive_transaction) &&
          read_text_file(root_inclusive / "stitching_canvas_provenance") == "replacement\n",
      "legacy PREPARED recovery must not promote a root-inclusive manifest into the old generation");
  fs::remove_all(root_inclusive);

  const fs::path torn_root = root / "legacy-torn-manifest";
  const fs::path torn_transaction = torn_root / ".hstream-stitch-torn";
  fs::create_directories(torn_root);
  write_legacy_transaction_fixture(torn_root, torn_transaction);
  std::ofstream(torn_root / "mapping_0000.tif", std::ios::trunc) << "replacement\n";
  std::ofstream(torn_transaction / "previous_artifacts", std::ios::trunc) << "mapping_000";
  std::ofstream(torn_transaction / "state") << "PREPARED\n";
  const auto torn_recovery = hm::stitching::HuginProject::Recover(torn_root);
  ok &= expect(
      !torn_recovery.ok() && fs::exists(torn_transaction) &&
          read_text_file(torn_root / "mapping_0000.tif") == "replacement\n",
      "an unversioned PREPARED journal with a torn manifest must fail closed");
  fs::remove_all(torn_root);

  const fs::path unsafe_root = root / "legacy-unsafe-rollback";
  const fs::path unsafe_transaction = unsafe_root / ".hstream-stitch-unsafe";
  fs::create_directories(unsafe_root);
  write_legacy_transaction_fixture(unsafe_root, unsafe_transaction);
  fs::remove(unsafe_transaction / "previous" / "hm_project.pto");
  std::ofstream(unsafe_root / "hm_project.pto", std::ios::trunc) << "replacement\n";
  {
    std::ofstream prior(unsafe_transaction / "previous_artifacts");
    for (const std::string& name : artifact_names)
      prior << name << '\n';
    std::ofstream(unsafe_transaction / "state") << "ROLLING_BACK\n";
  }
  const auto unsafe_recovery = hm::stitching::HuginProject::Recover(unsafe_root);
  ok &= expect(
      !unsafe_recovery.ok() && fs::exists(unsafe_transaction) &&
          read_text_file(unsafe_root / "hm_project.pto") == "replacement\n",
      "legacy ROLLING_BACK recovery must fail closed instead of accepting a mixed generation");
  fs::remove_all(unsafe_root);

  const fs::path torn_version_root = root / "torn-journal-version";
  const fs::path torn_version_transaction = torn_version_root / ".hstream-stitch-torn-version";
  fs::create_directories(torn_version_root);
  write_legacy_transaction_fixture(torn_version_root, torn_version_transaction);
  {
    std::ofstream prior(torn_version_transaction / "previous_artifacts");
    for (const std::string& name : artifact_names)
      prior << name << '\n';
  }
  std::ofstream(torn_version_root / "mapping_0000.tif", std::ios::trunc) << "replacement\n";
  std::ofstream(torn_version_transaction / "journal_version") << '2';
  std::ofstream(torn_version_transaction / "state") << "ROLLING_BACK\n";
  const auto torn_version_recovery = hm::stitching::HuginProject::Recover(torn_version_root);
  ok &= expect(
      !torn_version_recovery.ok() && fs::exists(torn_version_transaction) &&
          read_text_file(torn_version_root / "mapping_0000.tif") == "replacement\n",
      "a torn journal version must fail closed without consuming intact backups");
  fs::remove_all(torn_version_root);

  const fs::path oversized_manifest_root = root / "oversized-stitch-manifest";
  const fs::path oversized_manifest_transaction = oversized_manifest_root / ".hstream-stitch-oversized";
  fs::create_directories(oversized_manifest_transaction / "previous");
  std::ofstream(oversized_manifest_root / "sentinel") << "preserved\n";
  std::ofstream(oversized_manifest_transaction / "artifacts") << std::string(5000, 'x');
  std::ofstream(oversized_manifest_transaction / "journal_version") << "2\n";
  std::ofstream(oversized_manifest_transaction / "state") << "PREPARED\n";
  const auto oversized_manifest_recovery = hm::stitching::HuginProject::Recover(oversized_manifest_root);
  ok &= expect(
      !oversized_manifest_recovery.ok() && fs::exists(oversized_manifest_transaction) &&
          read_text_file(oversized_manifest_root / "sentinel") == "preserved\n",
      "an oversized stitch artifact manifest must fail before parsing or root mutation");
  fs::remove_all(oversized_manifest_root);

  const fs::path oversized_prior_root = root / "oversized-prior-manifest";
  const fs::path oversized_prior_transaction = oversized_prior_root / ".hstream-stitch-oversized-prior";
  fs::create_directories(oversized_prior_root);
  write_legacy_transaction_fixture(oversized_prior_root, oversized_prior_transaction);
  std::ofstream(oversized_prior_transaction / "previous_artifacts") << std::string(5000, 'x');
  std::ofstream(oversized_prior_transaction / "journal_version") << "2\n";
  std::ofstream(oversized_prior_transaction / "state") << "PREPARED\n";
  const auto oversized_prior_recovery = hm::stitching::HuginProject::Recover(oversized_prior_root);
  ok &= expect(
      !oversized_prior_recovery.ok() && fs::exists(oversized_prior_transaction) &&
          read_text_file(oversized_prior_root / "hm_project.pto") == "old-hm_project.pto\n",
      "an oversized prior-artifact manifest must fail before rollback mutation");
  fs::remove_all(oversized_prior_root);

  const fs::path provenance_path = root / "game" / "stitching_canvas_provenance";
  const std::string valid_provenance = read_text_file(provenance_path);
  const auto valid_provenance_time = fs::last_write_time(provenance_path);
  std::ofstream(provenance_path, std::ios::binary | std::ios::trunc) << std::string(5000, 'x');
  fs::last_write_time(provenance_path, valid_provenance_time);
  auto oversized_provenance_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
  ok &= expect(oversized_provenance_lock.ok(), "oversized canvas provenance fixture must lock");
  if (oversized_provenance_lock.ok()) {
    const auto parsed = hm::stitching::HuginProject::ReadCanvasProvenance(root / "game", **oversized_provenance_lock);
    const auto compatible = hm::stitching::check_canvas_constraint_locked(root / "game", 0);
    ok &= expect(
        !parsed.ok() && compatible.ok() && compatible->requires_regeneration,
        "oversized canvas provenance must be rejected by both parsers without an unbounded read");
  }
  if (oversized_provenance_lock.ok())
    oversized_provenance_lock->reset();
  std::ofstream(provenance_path, std::ios::binary | std::ios::trunc) << valid_provenance;
  fs::last_write_time(provenance_path, valid_provenance_time);

  const fs::path oversized_generation_root = root / "oversized-generation-artifact";
  std::error_code oversized_copy_error;
  fs::copy(
      root / "game",
      oversized_generation_root,
      fs::copy_options::recursive | fs::copy_options::copy_symlinks,
      oversized_copy_error);
  const int oversized_mapping =
      ::open((oversized_generation_root / "mapping_0000_x.tif").c_str(), O_WRONLY | O_CLOEXEC);
  const bool oversized_mapping_written = oversized_mapping >= 0 && ::ftruncate(oversized_mapping, 2LL << 30) == 0;
  if (oversized_mapping >= 0)
    ::close(oversized_mapping);
  auto oversized_generation_lock = hm::stitching::HuginProject::RecoverAndLock(oversized_generation_root);
  const auto oversized_generation = oversized_generation_lock.ok()
      ? hm::stitching::HuginProject::GenerationId(oversized_generation_root, **oversized_generation_lock)
      : absl::StatusOr<std::string>(oversized_generation_lock.status());
  ::setenv("HM_TEST_STITCH_DISABLE_LINK_CLONE", "1", 1);
  const absl::Status oversized_rollback = hm::stitching::clone_or_copy_stitch_rollback_file(
      oversized_generation_root / "mapping_0000_x.tif", oversized_generation_root / "rollback-mapping.tif");
  ::unsetenv("HM_TEST_STITCH_DISABLE_LINK_CLONE");
  ok &= expect(
      !oversized_copy_error && oversized_mapping_written && absl::IsFailedPrecondition(oversized_generation.status()) &&
          absl::IsFailedPrecondition(oversized_rollback) &&
          !fs::exists(oversized_generation_root / "rollback-mapping.tif"),
      "generation fingerprinting and rollback must reject oversized sparse TIFFs before reading their payload");
  if (oversized_generation_lock.ok())
    oversized_generation_lock->reset();
  fs::remove_all(oversized_generation_root);

  const fs::path padded_generation_root = root / "padded-generation-artifact";
  std::error_code padded_copy_error;
  fs::copy(
      root / "game",
      padded_generation_root,
      fs::copy_options::recursive | fs::copy_options::copy_symlinks,
      padded_copy_error);
  const bool padded_mapping_written =
      !padded_copy_error && ::truncate((padded_generation_root / "mapping_0000_x.tif").c_str(), 32LL << 20) == 0;
  auto padded_generation_lock = hm::stitching::HuginProject::RecoverAndLock(padded_generation_root);
  const auto padded_bounds = padded_generation_lock.ok()
      ? hm::stitching::validate_stitch_generation_artifact_bounds_locked(padded_generation_root)
      : padded_generation_lock.status();
  const auto padded_generation = padded_generation_lock.ok()
      ? hm::stitching::HuginProject::GenerationId(padded_generation_root, **padded_generation_lock)
      : absl::StatusOr<std::string>(padded_generation_lock.status());
  ok &= expect(
      padded_mapping_written && absl::IsResourceExhausted(padded_bounds) &&
          absl::IsFailedPrecondition(padded_generation.status()),
      "dimension-derived TIFF ceilings must reject valid headers with large trailing padding before hashing");
  if (padded_generation_lock.ok())
    padded_generation_lock->reset();
  fs::remove_all(padded_generation_root);

  const fs::path growing_rollback_root = root / "growing-rollback-source";
  fs::create_directories(growing_rollback_root);
  const fs::path growing_source = growing_rollback_root / "source.bin";
  const fs::path growing_destination = growing_rollback_root / "rollback.bin";
  std::ofstream(growing_source, std::ios::binary | std::ios::trunc) << std::string(64 * 1024, 'a');
  absl::Status growing_rollback_status;
  ::setenv("HM_TEST_ROLLBACK_PRE_COPY_DELAY_MS", "100", 1);
  std::thread growing_rollback([&]() {
    growing_rollback_status = hm::stitching::snapshot_regular_file_for_rollback(
        growing_source, growing_destination, /*force_portable_fallback=*/true, 1024 * 1024);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  const bool rollback_source_grew = ::truncate(growing_source.c_str(), 2LL * 1024LL * 1024LL) == 0;
  growing_rollback.join();
  ::unsetenv("HM_TEST_ROLLBACK_PRE_COPY_DELAY_MS");
  ok &= expect(
      rollback_source_grew && absl::IsAborted(growing_rollback_status) && !fs::exists(growing_destination),
      "portable rollback snapshots must reject concurrent growth without publishing the copied prefix");
  fs::remove_all(growing_rollback_root);

  const std::string portable_metadata = generation_stat_identity(root / "game", true);
  const std::string adopted_v1_generation = "adopted-v1-generation\n";
  std::ofstream(root / "game" / "stitching_generation_id", std::ios::binary | std::ios::trunc)
      << "version=1\nlegacy-size=" << adopted_v1_generation.size() << "\nmetadata-size=" << portable_metadata.size()
      << '\n'
      << adopted_v1_generation << portable_metadata;
  auto v1_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
  ok &= expect(v1_lock.ok(), "version-1 Hugin generation fixture must lock");
  if (v1_lock.ok()) {
    ::setenv("HM_TEST_STITCH_FCHMOD_UNSUPPORTED", "1", 1);
    const auto first = hm::stitching::HuginProject::GenerationId(root / "game", **v1_lock);
    ::unsetenv("HM_TEST_STITCH_FCHMOD_UNSUPPORTED");
    const auto second = hm::stitching::HuginProject::GenerationId(root / "game", **v1_lock);
    ok &= expect(
        first.ok() && second.ok() && *first == adopted_v1_generation && *second == adopted_v1_generation &&
            read_text_file(root / "game" / "stitching_generation_id").rfind("version=3\n", 0) == 0,
        "version-1 generation identities must upgrade atomically without changing their effective generation");
  }
  if (v1_lock.ok())
    v1_lock->reset();

  const std::string token_v1_identity =
      "version=1\ntoken=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
  const std::string token_v1_generation =
      "hstream-stitch-generation-v1\nidentity-bytes=" + std::to_string(token_v1_identity.size()) + '\n' +
      token_v1_identity + "metadata-bytes=" + std::to_string(portable_metadata.size()) + '\n' + portable_metadata;
  std::ofstream(root / "game" / "stitching_generation_id", std::ios::binary | std::ios::trunc) << token_v1_identity;
  auto token_v1_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
  ok &= expect(token_v1_lock.ok(), "token-based version-1 Hugin generation fixture must lock");
  if (token_v1_lock.ok()) {
    const auto generation = hm::stitching::HuginProject::GenerationId(root / "game", **token_v1_lock);
    ok &= expect(
        generation.ok() && *generation == token_v1_generation &&
            read_text_file(root / "game" / "stitching_generation_id").rfind("version=3\n", 0) == 0,
        "token-based version-1 identities must preserve their previous effective generation during upgrade");
  }
  if (token_v1_lock.ok())
    token_v1_lock->reset();

  const std::string current_bindings = generation_stat_identity(root / "game", false);
  const std::string preserved_v2_generation = "preserved-v2-generation\n";
  std::ofstream(root / "game" / "stitching_generation_id", std::ios::binary | std::ios::trunc)
      << "version=2\nlogical-size=" << preserved_v2_generation.size() << "\nbindings-size=" << current_bindings.size()
      << '\n'
      << preserved_v2_generation << current_bindings;
  auto v2_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
  ok &= expect(v2_lock.ok(), "version-2 Hugin generation fixture must lock");
  if (v2_lock.ok()) {
    const auto generation = hm::stitching::HuginProject::GenerationId(root / "game", **v2_lock);
    ok &= expect(
        generation.ok() && *generation == preserved_v2_generation &&
            read_text_file(root / "game" / "stitching_generation_id").rfind("version=3\n", 0) == 0,
        "version-2 generation identities must upgrade without changing their effective generation");
  }
  if (v2_lock.ok())
    v2_lock->reset();

  ::setenv("HM_TEST_STITCH_UNRELIABLE_METADATA", "1", 1);
  auto weak_v3_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
  const auto weak_v3_generation = weak_v3_lock.ok()
      ? hm::stitching::HuginProject::GenerationId(root / "game", **weak_v3_lock)
      : absl::StatusOr<std::string>(weak_v3_lock.status());
  if (weak_v3_lock.ok())
    weak_v3_lock->reset();
  const bool weak_v3_child_matches =
      weak_v3_generation.ok() && generation_matches_in_child(root / "game", *weak_v3_generation);
  ::unsetenv("HM_TEST_STITCH_UNRELIABLE_METADATA");
  ok &= expect(
      weak_v3_generation.ok() && *weak_v3_generation == preserved_v2_generation && weak_v3_child_matches &&
          read_text_file(root / "game" / "stitching_generation_id").rfind("version=3\n", 0) == 0,
      "version-3 generations on weak filesystems must remain content-stable across processes");

  const fs::path replacement_root = root / "same-metadata-replacement";
  fs::create_directories(replacement_root);
  for (const std::string& name : artifact_names) {
    if (fs::is_regular_file(root / "game" / name))
      fs::copy_file(root / "game" / name, replacement_root / name);
  }
  fs::copy_file(root / "game" / "stitching_generation_id", replacement_root / "stitching_generation_id");
  auto replacement_lock = hm::stitching::HuginProject::RecoverAndLock(replacement_root);
  ok &= expect(replacement_lock.ok(), "same-metadata replacement fixture must lock");
  if (replacement_lock.ok()) {
    const auto before = hm::stitching::HuginProject::GenerationId(replacement_root, **replacement_lock);

    const fs::path unreliable_root = root / "unreliable-metadata-replacement";
    fs::create_directories(unreliable_root);
    for (const std::string& name : artifact_names) {
      if (fs::is_regular_file(replacement_root / name))
        fs::copy_file(replacement_root / name, unreliable_root / name);
    }
    fs::copy_file(replacement_root / "stitching_generation_id", unreliable_root / "stitching_generation_id");
    auto unreliable_lock = hm::stitching::HuginProject::RecoverAndLock(unreliable_root);
    ok &= expect(unreliable_lock.ok(), "unreliable-metadata generation fixture must lock");
    if (unreliable_lock.ok()) {
      const auto unreliable_before = hm::stitching::HuginProject::GenerationId(unreliable_root, **unreliable_lock);
      const std::string old_bindings = generation_stat_identity(unreliable_root, false);
      const fs::path unreliable_target = unreliable_root / "hm_project.pto";
      struct stat unreliable_metadata{};
      const bool unreliable_metadata_read = ::stat(unreliable_target.c_str(), &unreliable_metadata) == 0;
      std::fstream changed_file(unreliable_target, std::ios::in | std::ios::out | std::ios::binary);
      char first_byte = '\0';
      changed_file.read(&first_byte, 1);
      changed_file.seekp(0);
      first_byte = first_byte == 'x' ? 'y' : 'x';
      changed_file.write(&first_byte, 1);
      changed_file.close();
      timespec unreliable_times[2] = {};
      unreliable_times[0].tv_nsec = UTIME_OMIT;
      unreliable_times[1] = unreliable_metadata.st_mtim;
      const bool unreliable_timestamp_preserved =
          unreliable_metadata_read && ::utimensat(AT_FDCWD, unreliable_target.c_str(), unreliable_times, 0) == 0;
      const std::string new_bindings = generation_stat_identity(unreliable_root, false);
      std::string forged_identity = read_text_file(unreliable_root / "stitching_generation_id");
      const size_t binding_offset = forged_identity.find(old_bindings);
      const std::string old_size = "bindings-size=" + std::to_string(old_bindings.size());
      const size_t size_offset = forged_identity.find(old_size);
      const bool identity_forgeable = binding_offset != std::string::npos && size_offset != std::string::npos;
      if (identity_forgeable) {
        forged_identity.replace(binding_offset, old_bindings.size(), new_bindings);
        forged_identity.replace(size_offset, old_size.size(), "bindings-size=" + std::to_string(new_bindings.size()));
        std::ofstream(unreliable_root / "stitching_generation_id", std::ios::binary | std::ios::trunc)
            << forged_identity;
      }
      ::setenv("HM_TEST_STITCH_UNRELIABLE_METADATA", "1", 1);
      const auto unreliable_after = hm::stitching::HuginProject::GenerationId(unreliable_root, **unreliable_lock);
      const auto unreliable_repeat = hm::stitching::HuginProject::GenerationId(unreliable_root, **unreliable_lock);
      ::unsetenv("HM_TEST_STITCH_UNRELIABLE_METADATA");
      ok &= expect(
          unreliable_before.ok() && unreliable_timestamp_preserved && identity_forgeable && unreliable_after.ok() &&
              unreliable_repeat.ok() && *unreliable_after != *unreliable_before &&
              *unreliable_repeat == *unreliable_after,
          "unreliable filesystems must use a stable content-verified generation instead of trusting artifact metadata");
    }
    if (unreliable_lock.ok())
      unreliable_lock->reset();
    fs::remove_all(unreliable_root);

    const fs::path target = replacement_root / "hm_project.pto";
    struct stat metadata{};
    const std::string original = read_text_file(target);
    std::string changed = original;
    if (!changed.empty())
      changed.front() = changed.front() == 'x' ? 'y' : 'x';
    const fs::path temporary = replacement_root / "hm_project.replacement";
    std::ofstream(temporary, std::ios::binary) << changed;
    const bool metadata_read = ::stat(target.c_str(), &metadata) == 0;
    timespec times[2] = {};
    times[0].tv_nsec = UTIME_OMIT;
    times[1] = metadata.st_mtim;
    const bool timestamp_preserved = metadata_read && ::utimensat(AT_FDCWD, temporary.c_str(), times, 0) == 0;
    std::error_code replace_error;
    fs::rename(temporary, target, replace_error);
    const auto rebound = hm::stitching::rebind_stitch_generation_artifact(replacement_root, replacement_root);
    const auto after = hm::stitching::HuginProject::GenerationId(replacement_root, **replacement_lock);
    ok &= expect(
        before.ok() && *before == preserved_v2_generation && !original.empty() && changed.size() == original.size() &&
            timestamp_preserved && !replace_error && rebound.ok() && after.ok() && *after != *before,
        "rebinding must not certify a same-size same-mtime replacement under the old logical generation");
  }
  if (replacement_lock.ok())
    replacement_lock->reset();
  fs::remove_all(replacement_root);

  const fs::path unreadable_root = root / "unreadable-stitch-backups";
  const fs::path unreadable_transaction = unreadable_root / ".hstream-stitch-unreadable";
  fs::create_directories(unreadable_root);
  write_legacy_transaction_fixture(unreadable_root, unreadable_transaction);
  {
    std::ofstream prior(unreadable_transaction / "previous_artifacts");
    for (const std::string& name : artifact_names)
      prior << name << '\n';
  }
  std::ofstream(unreadable_transaction / "journal_version") << "2\n";
  std::ofstream(unreadable_transaction / "state") << "PREPARED\n";
  fs::permissions(unreadable_transaction / "previous", fs::perms::owner_exec, fs::perm_options::replace);
  const auto unreadable_recovery = hm::stitching::HuginProject::Recover(unreadable_root);
  ok &= expect(
      !unreadable_recovery.ok() && fs::exists(unreadable_transaction) &&
          read_text_file(unreadable_root / "hm_project.pto") == "old-hm_project.pto\n",
      "failed backup directory enumeration must preserve the journal and root generation");
  fs::permissions(unreadable_transaction / "previous", fs::perms::owner_all, fs::perm_options::replace);
  fs::remove_all(unreadable_root);

  const fs::path unreadable_journal_root = root / "unreadable-stitch-journal-root";
  const fs::path unreadable_journal = unreadable_journal_root / ".hstream-stitch-unreadable-root";
  fs::create_directories(unreadable_journal_root);
  write_legacy_transaction_fixture(unreadable_journal_root, unreadable_journal);
  std::ofstream(unreadable_journal / "state") << "PREPARED\n";
  fs::permissions(unreadable_journal_root, fs::perms::owner_write | fs::perms::owner_exec, fs::perm_options::replace);
  const auto unreadable_journal_recovery = hm::stitching::HuginProject::Recover(unreadable_journal_root);
  fs::permissions(unreadable_journal_root, fs::perms::owner_all, fs::perm_options::replace);
  ok &= expect(
      !unreadable_journal_recovery.ok() && fs::exists(unreadable_journal) &&
          read_text_file(unreadable_journal_root / "hm_project.pto") == "old-hm_project.pto\n",
      "failed journal directory enumeration must not skip a pending stitch transaction");
  fs::remove_all(unreadable_journal_root);

  const fs::path malformed = root / "game" / ".hstream-stitch-malformed";
  fs::create_directories(malformed / "previous");
  std::ofstream(malformed / "state") << "PREPARE\n";
  const auto malformed_recovery = hm::stitching::HuginProject::Recover(root / "game");
  ok &= expect(!malformed_recovery.ok(), "unknown Hugin transaction state must fail closed");
  ok &= expect(fs::exists(malformed), "unknown Hugin transaction state must preserve its journal");
  ok &= expect(
      fs::is_regular_file(root / "game" / "autooptimiser_out.pto"),
      "unknown Hugin transaction state must not touch the committed generation");
  fs::remove_all(malformed);

  const fs::path multiline = root / "game" / ".hstream-stitch-multiline";
  fs::create_directories(multiline / "previous");
  std::ofstream(multiline / "state") << "PREPARED\n\nCOMMITTED\n";
  const auto multiline_recovery = hm::stitching::HuginProject::Recover(root / "game");
  ok &= expect(!multiline_recovery.ok(), "multiline Hugin transaction state must fail closed");
  ok &= expect(fs::exists(multiline), "multiline Hugin transaction state must preserve its journal");
  ok &= expect(
      fs::is_regular_file(root / "game" / "autooptimiser_out.pto"),
      "multiline Hugin transaction state must not touch the committed generation");
  fs::remove_all(multiline);

  const fs::path nonregular = root / "game" / ".hstream-stitch-nonregular";
  fs::create_directories(nonregular / "state");
  const auto nonregular_recovery = hm::stitching::HuginProject::Recover(root / "game");
  ok &= expect(!nonregular_recovery.ok(), "non-regular Hugin transaction state must fail closed");
  ok &= expect(fs::exists(nonregular), "non-regular Hugin transaction state must preserve its journal");
  fs::remove_all(nonregular);

  const fs::path unprepared = root / "game" / ".hstream-stitch-unprepared";
  fs::create_directories(unprepared);
  std::ofstream(unprepared / "temporary") << "not published\n";
  ok &= expect(
      hm::stitching::HuginProject::Recover(root / "game").ok() && !fs::exists(unprepared),
      "unprepared Hugin staging without publication metadata must be cleaned");

  const fs::path committed = root / "game" / ".hstream-stitch-committed";
  fs::create_directories(committed);
  std::ofstream(committed / "state") << "COMMITTED\n";
  ok &= expect(
      hm::stitching::HuginProject::Recover(root / "game").ok() && !fs::exists(committed),
      "committed Hugin journal must be cleaned without rollback");

  const fs::path rolled_back = root / "game" / ".hstream-stitch-rolled-back";
  fs::create_directories(rolled_back / "previous");
  std::ofstream(rolled_back / "state") << "ROLLED_BACK\n";
  const auto project_before_rolled_back_cleanup = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(
      hm::stitching::HuginProject::Recover(root / "game").ok() && !fs::exists(rolled_back),
      "rolled-back Hugin journal must be cleanup-only");
  const auto project_after_rolled_back_cleanup = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(
      project_after_rolled_back_cleanup == project_before_rolled_back_cleanup,
      "rolled-back Hugin cleanup must not remove the restored generation");

  auto reader_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
  ok &= expect(reader_lock.ok(), "Hugin reader must acquire the artifact lock");
  std::atomic<bool> second_reader_entered{false};
  std::atomic<bool> second_reader_ok{false};
  std::thread second_reader([&] {
    auto lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
    second_reader_ok = lock.ok();
    second_reader_entered = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ok &= expect(!second_reader_entered, "Hugin publication lock must remain held while a reader decodes artifacts");
  if (reader_lock.ok())
    reader_lock->reset();
  second_reader.join();
  ok &= expect(second_reader_entered && second_reader_ok, "waiting Hugin reader must proceed after lock release");
  bool staging_left_behind = false;
  for (const auto& entry : fs::directory_iterator(root / "game")) {
    if (entry.is_directory() && entry.path().filename().string().rfind(".hstream-stitch-", 0) == 0)
      staging_left_behind = true;
  }
  ok &= expect(!staging_left_behind, "private Hugin staging directory must be cleaned");
  fs::remove_all(root);
  return ok ? 0 : 1;
}
