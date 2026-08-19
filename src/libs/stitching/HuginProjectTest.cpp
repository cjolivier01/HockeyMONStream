#include "hstream/src/libs/stitching/HuginProject.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>
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

} // namespace

int main() {
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
  ok &= expect(!hm::stitching::HuginProject::ParseProjection("p w12 h6\n").ok(), "missing projection must fail");
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
  const fs::path nona = root / "nona";
  const fs::path enblend = root / "enblend";
  const fs::path fixtures = root / "fixtures";
  fs::create_directories(fixtures);
  constexpr float boundary_resolution = 8033.26416015625f;
  ok &= expect(
      write_spatial_tiff_tags(fixtures / "mapping_0000.tif", 40, 32, 3490.974853515625f, boundary_resolution),
      "first fake mapping must exist");
  ok &= expect(
      write_spatial_tiff_tags(fixtures / "mapping_0001.tif", 40, 32, 3490.97509765625f, boundary_resolution),
      "second fake mapping must exist");
  ok &= expect(write_remap_pair(fixtures, "mapping_0000", 40, 32), "first fake CV_16U remap must exist");
  ok &= expect(write_remap_pair(fixtures, "mapping_0001", 40, 32), "second fake CV_16U remap must exist");
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
  ok &= expect(
      cv::imwrite((fixtures / "panorama.tif").string(), cv::Mat(32, 42, CV_8UC3, cv::Scalar(1, 2, 3))),
      "fake panorama must exist");
  ok &= expect(
      write_tool(
          pto_gen,
          "printf '%s\\n' \"$*\" > '" + pto_gen_args.string() +
              "'\n"
              "printf '%s\\n' '# hugin project file' 'p f2 w100 h50 v180' '# control points' "
              "'#hugin_optimizeReferenceImage 0' > hm_project.pto\n"),
      "fake pto_gen must be created");
  ok &= expect(
      write_tool(
          autooptimiser,
          "args=\"$*\"; align=0; level=0; select=0; quiet=0; scale=1; output=; input=\n"
          "while [ \"$#\" -gt 0 ]; do case \"$1\" in -a) align=1 ;; -l) level=1 ;; -s) select=1 ;; "
          "-q) quiet=1 ;; -x) shift; scale=$1 ;; -o) shift; output=$1 ;; -n) exit 91 ;; *) input=$1 ;; "
          "esac; shift; done\n"
          "test \"$align:$level:$select:$quiet:$output:$input\" = "
          "'1:1:1:1:autooptimiser_out.pto:hm_project.pto'\n"
          "awk -v scale=\"$scale\" '/^p / { w=int(100 * scale + 0.5); h=int(50 * scale + 0.5); "
          "sub(/w[0-9]+/, \"w\" w); sub(/h[0-9]+/, \"h\" h) } { print }' \"$input\" > \"$output\"\n"
          "printf '%s\\n' \"$args\" >> '" +
              autooptimiser_args.string() +
              "'\n"
              "printf '%s\\n' 'Average (rms) distance between Controlpoints' 'after 1 iteration(s): 1.25 units'\n"),
      "fake autooptimiser must be created");
  ok &= expect(
      write_tool(
          nona,
          "for file in mapping_0000.tif mapping_0000_x.tif mapping_0000_y.tif mapping_0001.tif "
          "mapping_0001_x.tif mapping_0001_y.tif; do cp '" +
              fixtures.string() + "/'$file \"$file\"; done\n"),
      "fake nona must be created");
  ok &= expect(
      write_tool(
          enblend,
          "cp '" + fixtures.string() +
              "/seam_file.png' seam_file.png\n"
              "cp '" +
              fixtures.string() + "/panorama.tif' panorama.tif\n"),
      "fake enblend must be created");
  ::setenv("HM_PTO_GEN", pto_gen.c_str(), 1);
  ::setenv("HM_AUTOOPTIMISER", autooptimiser.c_str(), 1);
  ::setenv("HM_NONA", nona.c_str(), 1);
  ::setenv("HM_ENBLEND", enblend.c_str(), 1);
  matches.clear();
  for (int i = 0; i < 16; ++i) {
    matches.push_back({{i + 0.25f, i + 1.5f}, {i + 2.75f, i + 3.125f}, 0.9f});
  }
  hm::stitching::HuginProject::Options options;
  options.max_canvas_dimension = 64;
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
        scaled.ok() && scaled->first == 64 && scaled->second == 32,
        "autooptimiser -x scaling must cap the Hugin canvas");
    ok &= expect(
        contents.find("p f2 ") != std::string::npos,
        "the projection selected by autooptimiser must not be replaced by a pano_modify pass");
    std::ifstream optimizer_invocations(autooptimiser_args);
    const std::string optimizer_args(
        (std::istreambuf_iterator<char>(optimizer_invocations)), std::istreambuf_iterator<char>());
    ok &= expect(
        optimizer_args.find("-a -l -s -q -o autooptimiser_out.pto hm_project.pto") != std::string::npos,
        "Hugin orchestration must request automatic alignment and projection selection");
    ok &= expect(
        optimizer_args.find("-a -l -s -q -x 0.64 -o autooptimiser_out.pto hm_project.pto") != std::string::npos,
        "oversized Hugin canvases must be retried through autooptimiser -x");
    ok &= expect(
        optimizer_args.find("-n") == std::string::npos,
        "Hugin orchestration must not request script-only optimization");
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
       }) {
    ok &= expect(fs::is_regular_file(root / "game" / artifact), "configured Hugin artifact must be published");
  }

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
  options.expected_invalidation_id.clear();
  options.progress = {};
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
  ok &= expect(
      write_remap_pair(fixtures, "mapping_0000", 40, 32, true) &&
          write_remap_pair(fixtures, "mapping_0001", 40, 32, true),
      "degenerate remap fixtures must exist");
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

  const fs::path interrupted = root / "game" / ".hstream-stitch-interrupted";
  fs::create_directories(interrupted / "previous");
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
  };
  std::vector<fs::file_time_type> expected_mtimes;
  const auto generation_start = fs::file_time_type::clock::now() - std::chrono::minutes(1);
  {
    std::ofstream manifest(interrupted / "artifacts");
    for (size_t index = 0; index < artifact_names.size(); ++index) {
      const std::string& name = artifact_names[index];
      const auto modified = generation_start + std::chrono::seconds(index);
      fs::last_write_time(root / "game" / name, modified);
      expected_mtimes.push_back(fs::last_write_time(root / "game" / name));
      manifest << name << '\n';
      fs::copy_file(root / "game" / name, interrupted / "previous" / name);
      fs::last_write_time(interrupted / "previous" / name, expected_mtimes.back());
    }
    std::ofstream(interrupted / "state") << "PREPARED\n";
  }
  std::ofstream(root / "game" / "mapping_0000.tif", std::ios::trunc) << 'x';
  fs::remove(root / "game" / "autooptimiser_out.pto");
  const auto recovered = hm::stitching::HuginProject::Recover(root / "game");
  ok &= expect(recovered.ok(), "prepared Hugin publication must recover after an interrupted publish");
  ok &= expect(!fs::exists(interrupted), "recovered Hugin transaction must be cleaned");
  ok &= expect(
      fs::file_size(root / "game" / "mapping_0000.tif") > 1 &&
          fs::is_regular_file(root / "game" / "autooptimiser_out.pto"),
      "Hugin recovery must restore the complete prior generation");
  for (size_t index = 0; index < artifact_names.size(); ++index) {
    ok &= expect(
        fs::last_write_time(root / "game" / artifact_names[index]) == expected_mtimes[index],
        "Hugin recovery must preserve dependency timestamps");
  }
  ok &= expect(
      fs::last_write_time(root / "game" / "right.png") < fs::last_write_time(root / "game" / "hm_project.pto") &&
          fs::last_write_time(root / "game" / "hm_project.pto") <
              fs::last_write_time(root / "game" / "autooptimiser_out.pto") &&
          fs::last_write_time(root / "game" / "autooptimiser_out.pto") <
              fs::last_write_time(root / "game" / "mapping_0000.tif"),
      "restored Hugin dependency ordering must remain usable");

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
    if (entry.path().filename().string().rfind(".hstream-stitch-", 0) == 0)
      staging_left_behind = true;
  }
  ok &= expect(!staging_left_behind, "private Hugin staging directory must be cleaned");
  fs::remove_all(root);
  return ok ? 0 : 1;
}
