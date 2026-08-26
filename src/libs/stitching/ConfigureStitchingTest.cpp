#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <tiffio.h>
#include <yaml-cpp/yaml.h>

#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool write_text_file(const fs::path& path, const std::string& contents) {
  std::ofstream out(path);
  if (!out.is_open()) {
    std::cerr << "Failed to open " << path << " for writing" << std::endl;
    return false;
  }
  out << contents;
  return true;
}

bool write_canvas_provenance(
    const fs::path& dir,
    size_t max_output_width,
    size_t width,
    size_t height,
    size_t source_width = 0,
    size_t source_height = 0,
    size_t max_canvas_dimension = 0,
    bool max_output_width_applied = false,
    bool max_canvas_dimension_applied = false) {
  source_width = source_width == 0 ? width : source_width;
  source_height = source_height == 0 ? height : source_height;
  return write_text_file(
      dir / "stitching_canvas_provenance",
      "version=2\nmax-output-width=" + std::to_string(max_output_width) + "\nmax-canvas-dimension=" +
          std::to_string(max_canvas_dimension) + "\nsource-canvas-width=" + std::to_string(source_width) +
          "\nsource-canvas-height=" + std::to_string(source_height) + "\ncanvas-width=" + std::to_string(width) +
          "\ncanvas-height=" + std::to_string(height) +
          "\nmax-output-width-applied=" + std::to_string(max_output_width_applied ? 1 : 0) +
          "\nmax-canvas-dimension-applied=" + std::to_string(max_canvas_dimension_applied ? 1 : 0) + "\n");
}

bool write_mapping_tiff(const fs::path& path, uint32_t width, uint32_t height, float x_px, float y_px) {
  TIFF* tif = TIFFOpen(path.c_str(), "w");
  if (!tif) {
    std::cerr << "Failed to open TIFF " << path << " for writing" << std::endl;
    return false;
  }

  const float xres = 1.0f;
  const float yres = 1.0f;
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, height);
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, xres);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, yres);
  TIFFSetField(tif, TIFFTAG_XPOSITION, x_px / xres);
  TIFFSetField(tif, TIFFTAG_YPOSITION, y_px / yres);

  std::vector<uint8_t> row(width, 0);
  for (uint32_t y = 0; y < height; ++y) {
    if (TIFFWriteScanline(tif, row.data(), y, 0) < 0) {
      std::cerr << "Failed to write TIFF scanline " << y << " in " << path << std::endl;
      TIFFClose(tif);
      return false;
    }
  }

  TIFFClose(tif);
  return true;
}

bool write_remap_tiff(const fs::path& path, uint32_t width, uint32_t height) {
  TIFF* tif = TIFFOpen(path.c_str(), "w");
  if (!tif) {
    std::cerr << "Failed to open TIFF " << path << " for writing" << std::endl;
    return false;
  }

  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, height);

  std::vector<uint16_t> row(width, 0);
  for (uint32_t y = 0; y < height; ++y) {
    if (TIFFWriteScanline(tif, row.data(), y, 0) < 0) {
      std::cerr << "Failed to write TIFF scanline " << y << " in " << path << std::endl;
      TIFFClose(tif);
      return false;
    }
  }

  TIFFClose(tif);
  return true;
}

bool write_mapping_tiff_without_position(const fs::path& path, uint32_t width, uint32_t height) {
  TIFF* tif = TIFFOpen(path.c_str(), "w");
  if (!tif) {
    std::cerr << "Failed to open TIFF " << path << " for writing" << std::endl;
    return false;
  }

  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, height);
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, 1.0f);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, 1.0f);

  std::vector<uint8_t> row(width, 0);
  for (uint32_t y = 0; y < height; ++y) {
    if (TIFFWriteScanline(tif, row.data(), y, 0) < 0) {
      std::cerr << "Failed to write TIFF scanline " << y << " in " << path << std::endl;
      TIFFClose(tif);
      return false;
    }
  }

  TIFFClose(tif);
  return true;
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

bool add_png_pixel_offset(const fs::path& path, int32_t x, int32_t y) {
  std::ifstream input(path, std::ios::binary);
  std::vector<unsigned char> png((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (png.size() < 33 || std::string(png.begin() + 12, png.begin() + 16) != "IHDR")
    return false;
  std::vector<unsigned char> chunk;
  append_big_endian_u32(&chunk, 9);
  chunk.insert(chunk.end(), {'o', 'F', 'F', 's'});
  append_big_endian_u32(&chunk, static_cast<uint32_t>(x));
  append_big_endian_u32(&chunk, static_cast<uint32_t>(y));
  chunk.push_back(0);
  append_big_endian_u32(&chunk, png_crc32(chunk.data() + 4, 13));
  png.insert(png.begin() + 33, chunk.begin(), chunk.end());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  return output.good();
}

void set_write_time(const fs::path& path, int seconds_after_base) {
  const auto base = fs::file_time_type::clock::now() - std::chrono::seconds(60);
  fs::last_write_time(path, base + std::chrono::seconds(seconds_after_base));
}

bool write_valid_stitching_artifacts(const fs::path& dir) {
  fs::create_directories(dir);
  if (!write_text_file(dir / "left.png", "left") || !write_text_file(dir / "right.png", "right") ||
      !write_text_file(dir / "hm_project.pto", "p f1 w160 h32\n") ||
      !write_text_file(dir / "autooptimiser_out.pto", "p f1 w160 h32\n")) {
    return false;
  }

  if (!write_mapping_tiff(dir / "mapping_0000.tif", 64, 32, 0.0f, 0.0f) ||
      !write_mapping_tiff(dir / "mapping_0001.tif", 64, 32, 96.0f, 0.0f)) {
    return false;
  }

  if (!write_remap_tiff(dir / "mapping_0000_x.tif", 64, 32) || !write_remap_tiff(dir / "mapping_0000_y.tif", 64, 32) ||
      !write_remap_tiff(dir / "mapping_0001_x.tif", 64, 32) || !write_remap_tiff(dir / "mapping_0001_y.tif", 64, 32)) {
    return false;
  }
  cv::Mat seam(32, 160, CV_8U, cv::Scalar(0));
  seam.colRange(80, seam.cols).setTo(255);
  if (!cv::imwrite((dir / "seam_file.png").string(), seam) || !add_png_pixel_offset(dir / "seam_file.png", 0, 0)) {
    return false;
  }
  if (!write_canvas_provenance(dir, /*max_output_width=*/0, /*width=*/160, /*height=*/32)) {
    return false;
  }

  set_write_time(dir / "left.png", 0);
  set_write_time(dir / "right.png", 0);
  set_write_time(dir / "hm_project.pto", 1);
  set_write_time(dir / "autooptimiser_out.pto", 2);
  for (const char* filename :
       {"mapping_0000.tif",
        "mapping_0000_x.tif",
        "mapping_0000_y.tif",
        "mapping_0001.tif",
        "mapping_0001_x.tif",
        "mapping_0001_y.tif"}) {
    set_write_time(dir / filename, 3);
  }

  return true;
}

bool expect_configured(const fs::path& dir, bool expected, const std::string& label) {
  auto configured = hm::stitching::is_stitching_configured(dir.string());
  if (!configured.ok()) {
    std::cerr << label << ": unexpected status: " << configured.status() << std::endl;
    return false;
  }
  if (configured.value() != expected) {
    std::cerr << label << ": expected is_stitching_configured=" << expected << ", got " << configured.value()
              << std::endl;
    return false;
  }
  return true;
}

bool run_configured_child(
    const fs::path& dir,
    const std::string& max_dimension,
    bool allow_oversized,
    bool expected,
    const std::string& label) {
  const pid_t pid = fork();
  if (pid < 0) {
    std::cerr << label << ": fork failed" << std::endl;
    return false;
  }
  if (pid == 0) {
    if (allow_oversized) {
      setenv("HM_ALLOW_OVERSIZED_LIVE_STITCH", "1", /*overwrite=*/1);
    } else {
      unsetenv("HM_ALLOW_OVERSIZED_LIVE_STITCH");
    }
    setenv("HM_MAX_LIVE_STITCH_EGL_DIMENSION", max_dimension.c_str(), /*overwrite=*/1);
    _exit(expect_configured(dir, expected, label) ? 0 : 1);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    std::cerr << label << ": waitpid failed" << std::endl;
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::cerr << label << ": child failed with status " << status << std::endl;
    return false;
  }
  return true;
}

bool run_scoreboard_selector_env_child(const fs::path& tmpdir) {
  const fs::path game_dir = tmpdir / "scoreboard-disabled-game";
  fs::create_directories(game_dir);
  setenv("HM_NO_SCOREBOARD", "1", /*overwrite=*/1);
  const auto native_status = hm::stitching::configure_scoreboard(game_dir.string());
  unsetenv("HM_NO_SCOREBOARD");
  if (!native_status.ok() || !hm::stitching::is_scoreboard_configured(game_dir.string())) {
    std::cerr << "native disabled scoreboard configuration failed: " << native_status << std::endl;
    return false;
  }
  const YAML::Node polygon =
      YAML::LoadFile((game_dir / "config.yaml").string())["rink"]["scoreboard"]["perspective_polygon"];
  for (size_t index = 0; index < 4; ++index) {
    if (!polygon[index].IsSequence() || polygon[index][0].as<int>() != 0 || polygon[index][1].as<int>() != 0) {
      std::cerr << "native disabled scoreboard sentinel is invalid" << std::endl;
      return false;
    }
  }
  return true;
}

bool expect_dependency_invalidation_report(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "stale_dependency_report";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir)) {
    std::cerr << "dependency invalidation report: failed to write test artifacts" << std::endl;
    return false;
  }
  set_write_time(dir / "hm_project.pto", -10);

  std::ostringstream captured;
  std::streambuf* old_cout = std::cout.rdbuf(captured.rdbuf());
  std::streambuf* old_cerr = std::cerr.rdbuf(captured.rdbuf());
  auto configured = hm::stitching::is_stitching_configured(dir.string());
  std::cout.rdbuf(old_cout);
  std::cerr.rdbuf(old_cerr);

  if (!configured.ok()) {
    std::cerr << "dependency invalidation report: unexpected status " << configured.status() << std::endl;
    return false;
  }
  if (configured.value()) {
    std::cerr << "dependency invalidation report: stale artifacts reported as configured" << std::endl;
    return false;
  }

  const std::string output = captured.str();
  const std::vector<std::string> expected = {
      "Dependency violations found at level(s): 1",
      "Dependency invalidation tree:",
      "hm_project.pto [invalid]",
      "oldest output",
      "is older than dependency",
      "autooptimiser_out.pto [invalidated downstream]",
      "mapping_0000.tif,mapping_0000_x.tif,mapping_0000_y.tif",
      "[invalidated downstream]",
      "depends on invalid upstream item",
  };
  for (const auto& needle : expected) {
    if (output.find(needle) == std::string::npos) {
      std::cerr << "dependency invalidation report: missing \"" << needle << "\" in output:\n" << output << std::endl;
      return false;
    }
  }
  return true;
}

bool expect_clean_preserves_unrelated_config(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "clean_config_preservation";
  fs::remove_all(dir);
  fs::create_directories(dir);
  if (!write_text_file(
          dir / "config.yaml",
          R"(keep:
  nested: preserved
game:
  name: preservation-test
  stitching:
    frame_offsets: [1, 2]
stitching:
  control_points: [[1, 2], [3, 4]]
rink:
  scoreboard:
    perspective_polygon: [[0, 0], [1, 1]]
)")) {
    return false;
  }

  const auto status = hm::stitching::clean_stitching_artifacts(dir.string());
  if (!status.ok()) {
    std::cerr << "config preservation: cleaning failed: " << status << std::endl;
    return false;
  }

  const YAML::Node config = YAML::LoadFile((dir / "config.yaml").string());
  if (!config["keep"] || config["keep"]["nested"].as<std::string>("") != "preserved" || !config["game"] ||
      config["game"]["name"].as<std::string>("") != "preservation-test") {
    std::cerr << "config preservation: unrelated keys were changed:\n" << config << std::endl;
    return false;
  }
  if (config["stitching"] || config["game"]["stitching"] || config["rink"]) {
    std::cerr << "config preservation: cleanable keys remain:\n" << config << std::endl;
    return false;
  }
  return true;
}

bool expect_control_point_clean_preserves_upstream_dependencies(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "control_point_clean_dependencies";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir) || !write_text_file(dir / "s.png", "stitched") ||
      !write_text_file(dir / "rink_mask_0.png", "mask") || !write_text_file(dir / "matches.png", "matches") ||
      !write_text_file(
          dir / "config.yaml",
          R"(game:
  videos:
    left: [cam1/GX010001.MP4]
    right: [cam2/GX010002.MP4]
  stitching:
    frame_offsets:
      left: 3
      right: 0
stitching:
  control_points: [[1, 2], [3, 4]]
rink:
  scoreboard:
    perspective_polygon: [[0, 0], [1, 1]]
hstream_ui:
  stitching_calibration:
    control_points: 750
    status: pending
    stale_from: features
    artifacts_invalidated: false
    invalidation_id: current-clean
)")) {
    return false;
  }

  const auto superseded =
      hm::stitching::clean_stitching_artifacts_from_control_points(dir.string(), "delayed-stale-clean");
  if (superseded.code() != absl::StatusCode::kAborted || !fs::exists(dir / "hm_project.pto") ||
      !fs::exists(dir / "mapping_0000.tif")) {
    std::cerr << "superseded cleanup deleted a newer artifact generation: " << superseded << std::endl;
    return false;
  }

  const auto cleanup_before = hm::stitching::is_stitching_invalidation_cleanup_applied(dir.string(), "current-clean");
  if (!cleanup_before.ok() || cleanup_before.value()) {
    std::cerr << "current cleanup state was not revalidated before cleaning: " << cleanup_before.status() << std::endl;
    return false;
  }

  const auto status = hm::stitching::clean_stitching_artifacts_from_control_points(dir.string(), "current-clean");
  if (!status.ok()) {
    std::cerr << "control-point dependency clean failed: " << status << std::endl;
    return false;
  }
  const YAML::Node config = YAML::LoadFile((dir / "config.yaml").string());
  const bool upstream_preserved = fs::exists(dir / "left.png") && fs::exists(dir / "right.png") &&
      config["game"]["videos"]["left"] && config["game"]["videos"]["right"] &&
      config["game"]["stitching"]["frame_offsets"];
  const bool downstream_removed = !fs::exists(dir / "hm_project.pto") && !fs::exists(dir / "autooptimiser_out.pto") &&
      !fs::exists(dir / "mapping_0000.tif") && !fs::exists(dir / "mapping_0001.tif") &&
      !fs::exists(dir / "matches.png") && !fs::exists(dir / "s.png") && !fs::exists(dir / "rink_mask_0.png") &&
      !config["stitching"] && !config["rink"];
  const YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
  if (!upstream_preserved || !downstream_removed || !calibration || calibration["control_points"].as<int>(0) != 750 ||
      calibration["stale_from"].as<std::string>("") != "features") {
    std::cerr << "control-point clean did not preserve the dependency boundary:\n" << config << std::endl;
    return false;
  }
  YAML::Node precleaned = YAML::Clone(config);
  precleaned["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = true;
  const auto published = hm::stitching::publish_game_config(dir, YAML::Dump(precleaned) + "\n");
  const auto cleanup_after = hm::stitching::is_stitching_invalidation_cleanup_applied(dir.string(), "current-clean");
  if (!published.ok() || !cleanup_after.ok() || !cleanup_after.value()) {
    std::cerr << "completed cleanup state was not revalidated: publish=" << published
              << " validation=" << cleanup_after.status() << std::endl;
    return false;
  }
  return true;
}

bool expect_clean_waits_for_transaction_locks(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "clean_locking";
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::ofstream(dir / "config.yaml") << "unrelated: true\n";

  auto expect_wait = [&](auto held_lock, const char* message) {
    if (!held_lock.ok())
      return false;
    std::atomic<bool> finished{false};
    absl::Status clean_status;
    std::thread cleaner([&]() {
      clean_status = hm::stitching::clean_stitching_artifacts(dir.string());
      finished = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool waited = !finished.load();
    held_lock->reset();
    cleaner.join();
    if (!waited || !clean_status.ok()) {
      std::cerr << message << ": waited=" << waited << " status=" << clean_status << std::endl;
      return false;
    }
    return true;
  };

  if (!expect_wait(hm::stitching::HuginProject::RecoverAndLock(dir), "clean must wait for the Hugin lock"))
    return false;
  return expect_wait(
      hm::stitching::GameConfigTransactionLock::Acquire(dir), "clean must wait for the config/rink transaction lock");
}

bool expect_canvas_size_waits_for_hugin_lock(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "canvas_size_locking";
  fs::remove_all(dir);
  fs::create_directories(dir);
  if (!write_mapping_tiff(dir / "mapping_0000.tif", 64, 32, 0.0f, 0.0f) ||
      !write_mapping_tiff(dir / "mapping_0001.tif", 64, 32, 32.0f, 0.0f)) {
    return false;
  }

  auto held_lock = hm::stitching::HuginProject::RecoverAndLock(dir);
  if (!held_lock.ok())
    return false;
  std::atomic<bool> finished{false};
  absl::Status canvas_status;
  size_t canvas_width = 0;
  size_t canvas_height = 0;
  std::thread reader([&]() {
    auto canvas = hm::stitching::stitching_canvas_size(dir.string());
    canvas_status = canvas.status();
    if (canvas.ok()) {
      canvas_width = canvas->width;
      canvas_height = canvas->height;
    }
    finished = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const bool waited = !finished.load();
  held_lock->reset();
  reader.join();
  if (!waited || !canvas_status.ok() || canvas_width != 96 || canvas_height != 32) {
    std::cerr << "canvas sizing must hold the Hugin artifact lock: waited=" << waited << " status=" << canvas_status
              << " size=" << canvas_width << 'x' << canvas_height << std::endl;
    return false;
  }
  return true;
}

bool expect_legacy_seam_generation_rejects_oversized_tiff(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "oversized_legacy_tiff";
  fs::remove_all(dir);
  fs::create_directories(dir);
  if (!write_mapping_tiff(dir / "mapping_0000.tif", 40000, 1, 0.0f, 0.0f) ||
      !write_mapping_tiff(dir / "mapping_0001.tif", 1, 1, 0.0f, 0.0f)) {
    return false;
  }
  setenv("HM_ALLOW_HARD_SEAM_FALLBACK", "1", /*overwrite=*/1);
  const auto status = hm::stitching::maybe_create_default_seam_file(dir.string());
  unsetenv("HM_ALLOW_HARD_SEAM_FALLBACK");
  if (!absl::IsResourceExhausted(status) || fs::exists(dir / "seam_file.png")) {
    std::cerr << "oversized legacy TIFF must fail before seam allocation: " << status << std::endl;
    return false;
  }
  return true;
}

bool expect_hard_seam_generation_requires_opt_in(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "hard_seam_opt_in";
  fs::remove_all(dir);
  fs::create_directories(dir);
  if (!write_mapping_tiff(dir / "mapping_0000.tif", 64, 32, 0.0f, 0.0f) ||
      !write_mapping_tiff(dir / "mapping_0001.tif", 64, 32, 32.0f, 0.0f)) {
    return false;
  }

  unsetenv("HM_ALLOW_HARD_SEAM_FALLBACK");
  const auto disabled = hm::stitching::maybe_create_default_seam_file(dir.string());
  if (!absl::IsFailedPrecondition(disabled) ||
      std::string(disabled.message()).find("HM_ALLOW_HARD_SEAM_FALLBACK=1") == std::string::npos ||
      fs::exists(dir / "seam_file.png")) {
    std::cerr << "hard seam must fail closed without explicit opt-in: " << disabled << std::endl;
    return false;
  }

  setenv("HM_ALLOW_HARD_SEAM_FALLBACK", "1", /*overwrite=*/1);
  const auto enabled = hm::stitching::maybe_create_default_seam_file(dir.string());
  unsetenv("HM_ALLOW_HARD_SEAM_FALLBACK");
  if (!enabled.ok() || !fs::is_regular_file(dir / "seam_file.png")) {
    std::cerr << "hard seam opt-in must permit fallback generation: " << enabled << std::endl;
    return false;
  }
  return true;
}

bool expect_cropped_enblend_seam_is_normalized(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "cropped_enblend_seam";
  fs::remove_all(dir);
  fs::create_directories(dir);
  if (!write_mapping_tiff(dir / "mapping_0000.tif", 64, 32, 0.0f, 0.0f) ||
      !write_mapping_tiff(dir / "mapping_0001.tif", 64, 32, 32.0f, 0.0f)) {
    return false;
  }
  cv::Mat seam(30, 90, CV_8U, cv::Scalar(0));
  seam.colRange(45, seam.cols).setTo(255);
  if (!cv::imwrite((dir / "seam_file.png").string(), seam) || !add_png_pixel_offset(dir / "seam_file.png", 0, 0)) {
    return false;
  }

  unsetenv("HM_ALLOW_HARD_SEAM_FALLBACK");
  const auto status = hm::stitching::maybe_create_default_seam_file(dir.string());
  const cv::Mat preserved = cv::imread((dir / "seam_file.png").string(), cv::IMREAD_GRAYSCALE);
  cv::Mat expected;
  cv::copyMakeBorder(seam, expected, 0, 2, 0, 6, cv::BORDER_REPLICATE);
  if (!status.ok() || preserved.size() != cv::Size(96, 32) || cv::norm(preserved, expected, cv::NORM_INF) != 0) {
    std::cerr << "cropped enblend seam within the mapping canvas must be normalized: " << status << std::endl;
    return false;
  }
  return true;
}

bool expect_origin_zero_cropped_enblend_seam_is_scaled_for_cap(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "origin_zero_cropped_enblend_seam_cap";
  fs::remove_all(dir);
  fs::create_directories(dir);
  if (!write_mapping_tiff(dir / "mapping_0000.tif", 64, 32, 0.0f, 0.0f) ||
      !write_mapping_tiff(dir / "mapping_0001.tif", 64, 32, 32.0f, 0.0f)) {
    return false;
  }
  cv::Mat seam(30, 90, CV_8U, cv::Scalar(0));
  seam.colRange(45, seam.cols).setTo(255);
  if (!cv::imwrite((dir / "seam_file.png").string(), seam) || !add_png_pixel_offset(dir / "seam_file.png", 0, 0)) {
    return false;
  }

  unsetenv("HM_ALLOW_HARD_SEAM_FALLBACK");
  const auto status = hm::stitching::maybe_create_default_seam_file(dir.string(), /*max_output_width=*/48);
  const cv::Mat preserved = cv::imread((dir / "seam_file.png").string(), cv::IMREAD_GRAYSCALE);
  cv::Mat scaled;
  cv::resize(seam, scaled, cv::Size(45, 15), 0.0, 0.0, cv::INTER_NEAREST);
  cv::Mat expected;
  cv::copyMakeBorder(scaled, expected, 0, 1, 0, 3, cv::BORDER_REPLICATE);
  if (!status.ok() || preserved.size() != cv::Size(48, 16) || cv::norm(preserved, expected, cv::NORM_INF) != 0) {
    std::cerr << "origin-zero cropped enblend seam must be scaled and padded for capped stitching: " << status
              << std::endl;
    return false;
  }
  return true;
}

bool expect_effective_size_offset_seam_is_not_scaled_again(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "effective_size_offset_seam_cap";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir)) {
    return false;
  }
  if (!write_mapping_tiff(dir / "mapping_0000.tif", 40, 16, 0.0f, 0.0f) ||
      !write_mapping_tiff(dir / "mapping_0001.tif", 40, 16, 40.0f, 0.0f)) {
    return false;
  }
  if (!write_remap_tiff(dir / "mapping_0000_x.tif", 40, 16) || !write_remap_tiff(dir / "mapping_0000_y.tif", 40, 16) ||
      !write_remap_tiff(dir / "mapping_0001_x.tif", 40, 16) || !write_remap_tiff(dir / "mapping_0001_y.tif", 40, 16)) {
    return false;
  }
  if (!write_canvas_provenance(
          dir,
          /*max_output_width=*/80,
          /*width=*/80,
          /*height=*/16,
          /*source_width=*/160,
          /*source_height=*/32,
          /*max_canvas_dimension=*/0,
          /*max_output_width_applied=*/true)) {
    return false;
  }
  cv::Mat seam(16, 80, CV_8U, cv::Scalar(0));
  seam.colRange(40, seam.cols).setTo(255);
  if (!cv::imwrite((dir / "seam_file.png").string(), seam) || !add_png_pixel_offset(dir / "seam_file.png", 0, 0)) {
    return false;
  }

  const auto configured = hm::stitching::is_stitching_configured(dir.string(), /*max_output_width=*/80);
  unsetenv("HM_ALLOW_HARD_SEAM_FALLBACK");
  const auto status = hm::stitching::maybe_create_default_seam_file(dir.string(), /*max_output_width=*/80);
  const cv::Mat preserved = cv::imread((dir / "seam_file.png").string(), cv::IMREAD_GRAYSCALE);
  if (!configured.ok() || !*configured || !status.ok() || preserved.size() != seam.size() ||
      cv::norm(preserved, seam, cv::NORM_INF) != 0) {
    std::cerr << "effective-size capped seam with oFFs origin must not be scaled a second time: configured="
              << configured.status() << " status=" << status << std::endl;
    return false;
  }
  return true;
}

bool expect_runtime_validation_normalizes_cropped_seam(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "runtime_validation_cropped_seam";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir)) {
    return false;
  }
  cv::Mat seam(30, 150, CV_8U, cv::Scalar(0));
  seam.colRange(75, seam.cols).setTo(255);
  if (!cv::imwrite((dir / "seam_file.png").string(), seam) || !add_png_pixel_offset(dir / "seam_file.png", 5, 1)) {
    return false;
  }

  auto artifacts = hm::stitching::lock_validated_stitching_artifacts(dir.string());
  const cv::Mat normalized = cv::imread((dir / "seam_file.png").string(), cv::IMREAD_GRAYSCALE);
  if (!artifacts.ok() || !artifacts->artifact_lock || artifacts->canvas_size.width != 160 ||
      artifacts->canvas_size.height != 32 || normalized.size() != cv::Size(160, 32)) {
    std::cerr << "runtime artifact validation must normalize a valid cropped seam: " << artifacts.status() << std::endl;
    return false;
  }
  return true;
}

bool expect_canvas_provenance_invalidates_cap_changes(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "canvas_provenance_cap_changes";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir) || !write_mapping_tiff(dir / "mapping_0000.tif", 40, 16, 0.0f, 0.0f) ||
      !write_mapping_tiff(dir / "mapping_0001.tif", 40, 16, 40.0f, 0.0f) ||
      !write_remap_tiff(dir / "mapping_0000_x.tif", 40, 16) || !write_remap_tiff(dir / "mapping_0000_y.tif", 40, 16) ||
      !write_remap_tiff(dir / "mapping_0001_x.tif", 40, 16) || !write_remap_tiff(dir / "mapping_0001_y.tif", 40, 16) ||
      !write_canvas_provenance(
          dir,
          /*max_output_width=*/80,
          /*width=*/80,
          /*height=*/16,
          /*source_width=*/160,
          /*source_height=*/32,
          /*max_canvas_dimension=*/0,
          /*max_output_width_applied=*/true)) {
    return false;
  }
  cv::Mat seam(16, 80, CV_8U, cv::Scalar(0));
  seam.colRange(40, seam.cols).setTo(255);
  if (!cv::imwrite((dir / "seam_file.png").string(), seam)) {
    return false;
  }

  const auto configured_at_generation_cap = hm::stitching::is_stitching_configured(dir.string(), 80);
  const auto configured_at_larger_cap = hm::stitching::is_stitching_configured(dir.string(), 160);
  const auto configured_at_auto = hm::stitching::is_stitching_configured(dir.string(), 0);
  if (!configured_at_generation_cap.ok() || !*configured_at_generation_cap || !configured_at_larger_cap.ok() ||
      *configured_at_larger_cap || !configured_at_auto.ok() || *configured_at_auto) {
    std::cerr << "canvas provenance must invalidate capped artifacts when the configured cap changes" << std::endl;
    return false;
  }
  return true;
}

bool expect_nonbinding_cap_changes_reuse_artifacts(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "nonbinding_canvas_provenance_cap_changes";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir) ||
      !write_canvas_provenance(
          dir,
          /*max_output_width=*/8192,
          /*width=*/160,
          /*height=*/32,
          /*source_width=*/160,
          /*source_height=*/32,
          /*max_canvas_dimension=*/0,
          /*max_output_width_applied=*/false)) {
    return false;
  }

  const auto configured_at_smaller_nonbinding_cap = hm::stitching::is_stitching_configured(dir.string(), 7000);
  const auto configured_at_auto = hm::stitching::is_stitching_configured(dir.string(), 0);
  if (!configured_at_smaller_nonbinding_cap.ok() || !*configured_at_smaller_nonbinding_cap ||
      !configured_at_auto.ok() || !*configured_at_auto) {
    std::cerr << "nonbinding output-width changes must reuse the published mapping generation" << std::endl;
    return false;
  }
  return true;
}

bool expect_missing_provenance_requires_migration(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "missing_canvas_provenance";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir)) {
    return false;
  }
  fs::remove(dir / "stitching_canvas_provenance");
  const auto configured = hm::stitching::is_stitching_configured(dir.string(), 0);
  const auto regeneration = hm::stitching::stitching_artifacts_require_canvas_regeneration(dir.string(), 0);
  if (!configured.ok() || *configured || !regeneration.ok() || !*regeneration) {
    std::cerr << "provenance-less artifacts must receive a one-time migration regeneration" << std::endl;
    return false;
  }
  return true;
}

bool expect_changed_applied_live_limit_requires_regeneration(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "changed_applied_live_limit";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir) ||
      !write_canvas_provenance(
          dir,
          /*max_output_width=*/0,
          /*width=*/160,
          /*height=*/32,
          /*source_width=*/320,
          /*source_height=*/64,
          /*max_canvas_dimension=*/160,
          /*max_output_width_applied=*/false,
          /*max_canvas_dimension_applied=*/true)) {
    return false;
  }
  unsetenv("HM_ALLOW_OVERSIZED_LIVE_STITCH");
  setenv("HM_MAX_LIVE_STITCH_EGL_DIMENSION", "160", /*overwrite=*/1);
  const auto configured_at_generation_limit = hm::stitching::is_stitching_configured(dir.string(), 0);
  setenv("HM_MAX_LIVE_STITCH_EGL_DIMENSION", "320", /*overwrite=*/1);
  const auto configured_at_relaxed_limit = hm::stitching::is_stitching_configured(dir.string(), 0);
  unsetenv("HM_MAX_LIVE_STITCH_EGL_DIMENSION");
  if (!configured_at_generation_limit.ok() || !*configured_at_generation_limit || !configured_at_relaxed_limit.ok() ||
      *configured_at_relaxed_limit) {
    std::cerr << "changing an applied live canvas limit must regenerate maps at the newly available size" << std::endl;
    return false;
  }
  return true;
}

bool expect_superseded_constraints_reuse_artifacts(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "superseded_canvas_constraints";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir) ||
      !write_canvas_provenance(
          dir,
          /*max_output_width=*/300,
          /*width=*/160,
          /*height=*/32,
          /*source_width=*/320,
          /*source_height=*/64,
          /*max_canvas_dimension=*/160,
          /*max_output_width_applied=*/true,
          /*max_canvas_dimension_applied=*/true)) {
    return false;
  }
  unsetenv("HM_ALLOW_OVERSIZED_LIVE_STITCH");
  setenv("HM_MAX_LIVE_STITCH_EGL_DIMENSION", "160", /*overwrite=*/1);
  const auto configured_with_smaller_superseded_cap = hm::stitching::is_stitching_configured(dir.string(), 240);
  const auto configured_with_auto_cap = hm::stitching::is_stitching_configured(dir.string(), 0);
  auto reusable_check = hm::stitching::lock_canvas_regeneration_check(dir.string(), 240);
  const bool reusable_check_ok = reusable_check.ok() && reusable_check->artifacts_compatible &&
      !reusable_check->requires_regeneration && reusable_check->artifact_lock;
  if (reusable_check.ok())
    reusable_check->artifact_lock.reset();
  const fs::path committed_transaction = dir / ".hstream-stitch-committed";
  const fs::path unprepared_transaction = dir / ".hstream-stitch-unprepared";
  std::error_code transaction_error;
  fs::create_directory(committed_transaction, transaction_error);
  const bool wrote_committed_transaction =
      !transaction_error && write_text_file(committed_transaction / "state", "COMMITTED\n");
  transaction_error.clear();
  fs::create_directory(unprepared_transaction, transaction_error);
  auto lightweight_reusable_check = hm::stitching::try_lock_canvas_constraint_check(dir, 240);
  const bool lightweight_reusable_check_ok = lightweight_reusable_check.ok() &&
      lightweight_reusable_check->artifacts_compatible && !lightweight_reusable_check->requires_regeneration &&
      lightweight_reusable_check->artifact_lock && wrote_committed_transaction && !transaction_error &&
      !fs::exists(committed_transaction) && !fs::exists(unprepared_transaction);
  if (lightweight_reusable_check.ok())
    lightweight_reusable_check->artifact_lock.reset();
  if (!write_canvas_provenance(
          dir,
          /*max_output_width=*/160,
          /*width=*/160,
          /*height=*/32,
          /*source_width=*/320,
          /*source_height=*/64,
          /*max_canvas_dimension=*/160,
          /*max_output_width_applied=*/true,
          /*max_canvas_dimension_applied=*/true)) {
    unsetenv("HM_MAX_LIVE_STITCH_EGL_DIMENSION");
    return false;
  }
  setenv("HM_MAX_LIVE_STITCH_EGL_DIMENSION", "320", /*overwrite=*/1);
  const auto configured_with_relaxed_tied_limit = hm::stitching::is_stitching_configured(dir.string(), 160);
  unsetenv("HM_MAX_LIVE_STITCH_EGL_DIMENSION");
  if (!reusable_check_ok || !lightweight_reusable_check_ok || !configured_with_smaller_superseded_cap.ok() ||
      !*configured_with_smaller_superseded_cap || !configured_with_auto_cap.ok() || !*configured_with_auto_cap ||
      !configured_with_relaxed_tied_limit.ok() || !*configured_with_relaxed_tied_limit) {
    std::cerr << "constraints superseded by an unchanged effective scale must reuse the published maps" << std::endl;
    return false;
  }
  return true;
}

bool expect_stale_mapping_dependencies_invalidate_canvas_cache(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "stale_mapping_canvas_cache";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir)) {
    return false;
  }
  set_write_time(dir / "left.png", 4);
  const auto regeneration = hm::stitching::stitching_artifacts_require_canvas_regeneration(dir.string(), 0);
  if (!regeneration.ok() || !*regeneration) {
    std::cerr << "stale existing mapping dependencies must invalidate canvas-relative rink caches" << std::endl;
    return false;
  }
  return true;
}

bool expect_canvas_regeneration_check_retains_generation_lock(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "canvas_regeneration_generation_lock";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir)) {
    return false;
  }
  auto check = hm::stitching::lock_canvas_regeneration_check(dir.string(), /*max_output_width=*/80);
  if (!check.ok() || check->artifacts_compatible || !check->requires_regeneration || !check->artifact_lock) {
    return false;
  }
  std::atomic<bool> finished{false};
  absl::Status lock_status;
  std::thread publisher([&]() {
    auto publication_lock = hm::stitching::HuginProject::RecoverAndLock(dir);
    lock_status = publication_lock.status();
    finished = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const bool waited = !finished.load();
  check->artifact_lock.reset();
  publisher.join();
  if (!waited || !lock_status.ok()) {
    std::cerr << "canvas cache invalidation must retain its reviewed Hugin generation" << std::endl;
    return false;
  }
  return true;
}

bool expect_width_cap_marks_native_canvas_for_regeneration(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "width_cap_canvas_regeneration";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir)) {
    return false;
  }
  const auto requires_regeneration =
      hm::stitching::stitching_artifacts_require_canvas_regeneration(dir.string(), /*max_output_width=*/80);
  if (!requires_regeneration.ok() || !*requires_regeneration) {
    std::cerr << "a width cap below the published canvas must invalidate canvas-relative rink data: "
              << requires_regeneration.status() << std::endl;
    return false;
  }
  return true;
}

bool expect_native_over_cap_mappings_are_not_configured(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "native_over_cap_mappings";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir)) {
    return false;
  }
  cv::Mat seam(16, 80, CV_8U, cv::Scalar(0));
  seam.colRange(40, seam.cols).setTo(255);
  if (!cv::imwrite((dir / "seam_file.png").string(), seam) || !add_png_pixel_offset(dir / "seam_file.png", 0, 0)) {
    return false;
  }

  const auto configured = hm::stitching::is_stitching_configured(dir.string(), /*max_output_width=*/80);
  if (!configured.ok() || *configured) {
    std::cerr << "native over-cap mapping artifacts must be regenerated for capped stitching: " << configured.status()
              << std::endl;
    return false;
  }
  return true;
}

bool expect_mismatched_capped_seam_is_rejected(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "mismatched_capped_seam";
  fs::remove_all(dir);
  fs::create_directories(dir);
  if (!write_mapping_tiff(dir / "mapping_0000.tif", 64, 32, 0.0f, 0.0f) ||
      !write_mapping_tiff(dir / "mapping_0001.tif", 64, 32, 32.0f, 0.0f)) {
    return false;
  }
  cv::Mat seam(20, 60, CV_8U, cv::Scalar(0));
  seam.colRange(30, seam.cols).setTo(255);
  if (!cv::imwrite((dir / "seam_file.png").string(), seam)) {
    return false;
  }

  unsetenv("HM_ALLOW_HARD_SEAM_FALLBACK");
  const auto status = hm::stitching::maybe_create_default_seam_file(dir.string(), /*max_output_width=*/48);
  const cv::Mat preserved = cv::imread((dir / "seam_file.png").string(), cv::IMREAD_GRAYSCALE);
  if (!absl::IsFailedPrecondition(status) || preserved.size() != seam.size() ||
      cv::norm(preserved, seam, cv::NORM_INF) != 0) {
    std::cerr << "mismatched capped seam must be rejected without being scaled again: " << status << std::endl;
    return false;
  }
  return true;
}

bool expect_mismatched_capped_seam_is_not_configured(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "mismatched_capped_seam_configured";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir)) {
    return false;
  }
  cv::Mat seam(20, 60, CV_8U, cv::Scalar(0));
  seam.colRange(30, seam.cols).setTo(255);
  if (!cv::imwrite((dir / "seam_file.png").string(), seam)) {
    return false;
  }

  const auto configured = hm::stitching::is_stitching_configured(dir.string(), /*max_output_width=*/80);
  if (!configured.ok() || *configured) {
    std::cerr << "mismatched capped seam must make capped stitching artifacts unconfigured: " << configured.status()
              << std::endl;
    return false;
  }
  return true;
}

bool expect_stale_capped_seam_is_not_configured_when_uncapped(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "stale_capped_seam_uncapped";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir)) {
    return false;
  }
  cv::Mat seam(20, 80, CV_8U, cv::Scalar(0));
  seam.colRange(40, seam.cols).setTo(255);
  if (!cv::imwrite((dir / "seam_file.png").string(), seam)) {
    return false;
  }

  const auto configured = hm::stitching::is_stitching_configured(dir.string(), /*max_output_width=*/0);
  if (!configured.ok() || *configured) {
    std::cerr << "stale capped seam must make uncapped stitching artifacts unconfigured: " << configured.status()
              << std::endl;
    return false;
  }
  return true;
}

bool expect_uniform_seam_is_not_configured(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "uniform_seam_configured";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir)) {
    return false;
  }
  cv::Mat seam(32, 160, CV_8U, cv::Scalar(255));
  if (!cv::imwrite((dir / "seam_file.png").string(), seam)) {
    return false;
  }

  const auto configured = hm::stitching::is_stitching_configured(dir.string(), /*max_output_width=*/0);
  auto lightweight = hm::stitching::try_lock_canvas_constraint_check(dir, /*max_output_width=*/0);
  const bool lightweight_rejects = lightweight.ok() && !lightweight->artifacts_compatible &&
      lightweight->requires_regeneration && lightweight->artifact_lock;
  if (lightweight.ok())
    lightweight->artifact_lock.reset();
  if (!configured.ok() || *configured || !lightweight_rejects) {
    std::cerr << "uniform same-size seam must invalidate runtime and lightweight stitching checks: "
              << configured.status() << std::endl;
    return false;
  }
  return true;
}

bool expect_missing_placement_tiff_is_not_configured(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "missing_placement_configured";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir) || !write_mapping_tiff_without_position(dir / "mapping_0001.tif", 64, 32)) {
    return false;
  }

  const auto configured = hm::stitching::is_stitching_configured(dir.string(), /*max_output_width=*/0);
  if (!configured.ok() || *configured) {
    std::cerr << "missing TIFF placement tags must make stitching artifacts unconfigured: " << configured.status()
              << std::endl;
    return false;
  }
  return true;
}

bool expect_mismatched_remap_headers_are_not_configured(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "mismatched_remap_headers_configured";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir)) {
    return false;
  }
  if (!write_mapping_tiff(dir / "mapping_0001_y.tif", 640, 320, 0.0f, 0.0f)) {
    return false;
  }

  const auto configured = hm::stitching::is_stitching_configured(dir.string(), /*max_output_width=*/160);
  auto lightweight = hm::stitching::try_lock_canvas_constraint_check(dir, /*max_output_width=*/160);
  const bool lightweight_rejects = lightweight.ok() && !lightweight->artifacts_compatible &&
      lightweight->requires_regeneration && lightweight->artifact_lock;
  if (lightweight.ok())
    lightweight->artifact_lock.reset();
  if (!configured.ok() || *configured || !lightweight_rejects) {
    std::cerr << "mismatched remap X/Y headers must invalidate runtime and lightweight stitching checks: "
              << configured.status() << std::endl;
    return false;
  }
  return true;
}

bool expect_placement_remap_size_mismatch_is_not_configured(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "placement_remap_size_mismatch_configured";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir) || !write_mapping_tiff(dir / "mapping_0001.tif", 32, 16, 96.0f, 0.0f)) {
    return false;
  }

  const auto configured = hm::stitching::is_stitching_configured(dir.string(), /*max_output_width=*/160);
  if (!configured.ok() || *configured) {
    std::cerr << "placement/remap size mismatch must make stitching artifacts unconfigured: " << configured.status()
              << std::endl;
    return false;
  }
  return true;
}

bool expect_stale_snapshot_publisher_is_rejected(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "stale_snapshot_publisher";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir))
    return false;

  auto hugin_lock = hm::stitching::HuginProject::RecoverAndLock(dir);
  if (!hugin_lock.ok())
    return false;
  auto first_hugin_generation = hm::stitching::HuginProject::GenerationId(dir, **hugin_lock);
  if (!first_hugin_generation.ok())
    return false;
  auto first_output_generation = hm::stitching::stitched_output_generation_id(*first_hugin_generation, 0.0, 160, 32);
  if (!first_output_generation.ok())
    return false;
  YAML::Node first_config(YAML::NodeType::Map);
  first_config["rink"]["stitched_output_generation"] = *first_output_generation;
  if (!write_text_file(dir / "config.yaml", YAML::Dump(first_config) + "\n"))
    return false;

  cv::Mat stale_snapshot(32, 160, CV_8UC3, cv::Scalar(0, 0, 255));
  absl::Status publisher_status;
  std::thread stale_publisher([&] {
    publisher_status = hm::stitching::save_stitched_image(dir.string(), stale_snapshot, *first_output_generation);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  {
    std::ofstream changed(dir / "autooptimiser_out.pto", std::ios::app);
    changed << "# replacement generation\n";
  }
  auto second_hugin_generation = hm::stitching::HuginProject::GenerationId(dir, **hugin_lock);
  if (!second_hugin_generation.ok()) {
    hugin_lock->reset();
    stale_publisher.join();
    return false;
  }
  auto second_output_generation = hm::stitching::stitched_output_generation_id(*second_hugin_generation, 0.0, 160, 32);
  if (!second_output_generation.ok()) {
    hugin_lock->reset();
    stale_publisher.join();
    return false;
  }
  auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(dir);
  if (!config_lock.ok()) {
    hugin_lock->reset();
    stale_publisher.join();
    return false;
  }
  YAML::Node second_config(YAML::NodeType::Map);
  second_config["rink"]["stitched_output_generation"] = *second_output_generation;
  if (!hm::stitching::publish_game_config(dir, YAML::Dump(second_config) + "\n").ok()) {
    config_lock->reset();
    hugin_lock->reset();
    stale_publisher.join();
    return false;
  }
  cv::Mat current_snapshot(32, 160, CV_8UC3, cv::Scalar(0, 255, 0));
  const fs::path current_temporary = dir / "s-current.png";
  std::error_code rename_error;
  if (!cv::imwrite(current_temporary.string(), current_snapshot)) {
    config_lock->reset();
    hugin_lock->reset();
    stale_publisher.join();
    return false;
  }
  fs::rename(current_temporary, dir / "s.png", rename_error);
  config_lock->reset();
  hugin_lock->reset();
  stale_publisher.join();

  const cv::Mat published = cv::imread((dir / "s.png").string(), cv::IMREAD_COLOR);
  if (!absl::IsAborted(publisher_status) || rename_error || published.empty() ||
      published.at<cv::Vec3b>(0, 0) != cv::Vec3b(0, 255, 0)) {
    std::cerr << "a stale frame publisher must not overwrite the current stitched-output generation: "
              << publisher_status << std::endl;
    return false;
  }
  return true;
}

void finish(const fs::path& tmpdir, int code) {
  fs::remove_all(tmpdir);
  _exit(code);
}

} // namespace

int main() {
  const fs::path tmpdir =
      fs::temp_directory_path() / ("configure_stitching_canvas_cap_test_" + std::to_string(::getpid()));
  fs::remove_all(tmpdir);
  if (!write_valid_stitching_artifacts(tmpdir)) {
    finish(tmpdir, 1);
  }

  if (!run_configured_child(tmpdir, "128", /*allow_oversized=*/false, false, "oversized canvas should be rejected")) {
    finish(tmpdir, 2);
  }

  if (!run_configured_child(tmpdir, "160", /*allow_oversized=*/false, true, "canvas at cap should be accepted")) {
    finish(tmpdir, 3);
  }

  if (!run_configured_child(
          tmpdir, "128", /*allow_oversized=*/true, true, "explicit oversized override should be accepted")) {
    finish(tmpdir, 4);
  }

  if (!run_scoreboard_selector_env_child(tmpdir)) {
    finish(tmpdir, 5);
  }

  if (!expect_dependency_invalidation_report(tmpdir)) {
    finish(tmpdir, 7);
  }

  if (!expect_clean_preserves_unrelated_config(tmpdir)) {
    finish(tmpdir, 8);
  }

  if (!expect_control_point_clean_preserves_upstream_dependencies(tmpdir)) {
    finish(tmpdir, 11);
  }

  if (!expect_clean_waits_for_transaction_locks(tmpdir)) {
    finish(tmpdir, 9);
  }

  if (!expect_canvas_size_waits_for_hugin_lock(tmpdir)) {
    finish(tmpdir, 24);
  }

  if (!expect_legacy_seam_generation_rejects_oversized_tiff(tmpdir)) {
    finish(tmpdir, 10);
  }

  if (!expect_hard_seam_generation_requires_opt_in(tmpdir)) {
    finish(tmpdir, 12);
  }

  if (!expect_cropped_enblend_seam_is_normalized(tmpdir)) {
    finish(tmpdir, 13);
  }

  if (!expect_origin_zero_cropped_enblend_seam_is_scaled_for_cap(tmpdir)) {
    finish(tmpdir, 18);
  }

  if (!expect_effective_size_offset_seam_is_not_scaled_again(tmpdir)) {
    finish(tmpdir, 19);
  }

  if (!expect_runtime_validation_normalizes_cropped_seam(tmpdir)) {
    finish(tmpdir, 25);
  }

  if (!expect_canvas_provenance_invalidates_cap_changes(tmpdir)) {
    finish(tmpdir, 26);
  }

  if (!expect_nonbinding_cap_changes_reuse_artifacts(tmpdir)) {
    finish(tmpdir, 28);
  }

  if (!expect_missing_provenance_requires_migration(tmpdir)) {
    finish(tmpdir, 29);
  }

  if (!expect_changed_applied_live_limit_requires_regeneration(tmpdir)) {
    finish(tmpdir, 30);
  }

  if (!expect_superseded_constraints_reuse_artifacts(tmpdir)) {
    finish(tmpdir, 33);
  }

  if (!expect_stale_mapping_dependencies_invalidate_canvas_cache(tmpdir)) {
    finish(tmpdir, 31);
  }

  if (!expect_canvas_regeneration_check_retains_generation_lock(tmpdir)) {
    finish(tmpdir, 32);
  }

  if (!expect_width_cap_marks_native_canvas_for_regeneration(tmpdir)) {
    finish(tmpdir, 27);
  }

  if (!expect_native_over_cap_mappings_are_not_configured(tmpdir)) {
    finish(tmpdir, 20);
  }

  if (!expect_mismatched_capped_seam_is_rejected(tmpdir)) {
    finish(tmpdir, 14);
  }

  if (!expect_mismatched_capped_seam_is_not_configured(tmpdir)) {
    finish(tmpdir, 15);
  }

  if (!expect_stale_capped_seam_is_not_configured_when_uncapped(tmpdir)) {
    finish(tmpdir, 16);
  }

  if (!expect_uniform_seam_is_not_configured(tmpdir)) {
    finish(tmpdir, 17);
  }

  if (!expect_missing_placement_tiff_is_not_configured(tmpdir)) {
    finish(tmpdir, 21);
  }

  if (!expect_mismatched_remap_headers_are_not_configured(tmpdir)) {
    finish(tmpdir, 22);
  }

  if (!expect_placement_remap_size_mismatch_is_not_configured(tmpdir)) {
    finish(tmpdir, 23);
  }

  if (!expect_stale_snapshot_publisher_is_rejected(tmpdir)) {
    finish(tmpdir, 34);
  }

  finish(tmpdir, 0);
}
