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
    bool max_canvas_dimension_applied = false,
    const std::string& mapping_backend = {},
    const std::string& projection = {},
    const std::string& projection_parameters = {},
    const hm::stitching::StitchProjectionFraming& projection_framing = {},
    const std::string& control_point_matcher = "superpoint-lightglue",
    const std::string& akaze_calibration_fingerprint = "not-applicable") {
  source_width = source_width == 0 ? width : source_width;
  source_height = source_height == 0 ? height : source_height;
  const bool algorithm_aware = !mapping_backend.empty() || !projection.empty();
  const bool parameter_aware = !projection_parameters.empty();
  if (algorithm_aware && (mapping_backend.empty() || projection.empty()))
    return false;
  if (parameter_aware && !algorithm_aware)
    return false;
  return write_text_file(
      dir / "stitching_canvas_provenance",
      std::string(algorithm_aware ? "version=6\n" : "version=2\n") + "max-output-width=" +
          std::to_string(max_output_width) + "\nmax-canvas-dimension=" + std::to_string(max_canvas_dimension) +
          "\nsource-canvas-width=" + std::to_string(source_width) +
          "\nsource-canvas-height=" + std::to_string(source_height) + "\ncanvas-width=" + std::to_string(width) +
          "\ncanvas-height=" + std::to_string(height) +
          "\nmax-output-width-applied=" + std::to_string(max_output_width_applied ? 1 : 0) +
          "\nmax-canvas-dimension-applied=" + std::to_string(max_canvas_dimension_applied ? 1 : 0) + "\n" +
          (algorithm_aware ? "mapping-backend=" + mapping_backend + "\nprojection=" + projection +
                   "\nprojection-parameters=" + (parameter_aware ? projection_parameters : "none") +
                   "\nprojection-auto-fov=" + (projection_framing.auto_fov ? "1" : "0") +
                   "\nprojection-horizontal-fov=" + std::to_string(projection_framing.horizontal_fov) +
                   "\nprojection-auto-canvas=" + (projection_framing.auto_canvas ? "1" : "0") +
                   "\nprojection-auto-crop=" + (projection_framing.auto_crop ? "1" : "0") + "\ncontrol-point-matcher=" +
                   control_point_matcher + "\nakaze-calibration-fingerprint=" + akaze_calibration_fingerprint + "\n"
                           : ""));
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

void set_big_endian_u32(std::vector<unsigned char>* output, size_t offset, uint32_t value) {
  (*output)[offset] = static_cast<unsigned char>(value >> 24);
  (*output)[offset + 1] = static_cast<unsigned char>(value >> 16);
  (*output)[offset + 2] = static_cast<unsigned char>(value >> 8);
  (*output)[offset + 3] = static_cast<unsigned char>(value);
}

bool corrupt_png_idat_with_valid_crc(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::vector<unsigned char> png((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  auto read_big_endian_u32 = [&](size_t offset) {
    return (static_cast<uint32_t>(png[offset]) << 24) | (static_cast<uint32_t>(png[offset + 1]) << 16) |
        (static_cast<uint32_t>(png[offset + 2]) << 8) | static_cast<uint32_t>(png[offset + 3]);
  };
  for (size_t offset = 8; offset + 12 <= png.size();) {
    const uint32_t length = read_big_endian_u32(offset);
    if (length > png.size() - offset - 12)
      return false;
    const size_t type_offset = offset + 4;
    const size_t data_offset = offset + 8;
    if (std::string(png.begin() + type_offset, png.begin() + type_offset + 4) == "IDAT" && length > 0) {
      std::fill(png.begin() + data_offset, png.begin() + data_offset + length, 0);
      set_big_endian_u32(&png, data_offset + length, png_crc32(png.data() + type_offset, length + 4));
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
      return output.good();
    }
    offset += static_cast<size_t>(length) + 12;
  }
  return false;
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

bool wait_for_test_marker(const fs::path& marker) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (fs::exists(marker))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
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

bool expect_mapping_algorithm_changes_require_regeneration(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "mapping-algorithm-provenance";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir))
    return false;
  YAML::Node config;
  config["stitching"]["mapping_backend"] = "nona";
  config["stitching"]["projection"] = "cylindrical";
  if (!write_text_file(dir / "config.yaml", YAML::Dump(config) + "\n"))
    return false;
  if (!expect_configured(dir, false, "legacy provenance must not mask a selected projection"))
    return false;
  if (!write_canvas_provenance(
          dir,
          /*max_output_width=*/0,
          /*width=*/160,
          /*height=*/32,
          /*source_width=*/0,
          /*source_height=*/0,
          /*max_canvas_dimension=*/0,
          /*max_output_width_applied=*/false,
          /*max_canvas_dimension_applied=*/false,
          "nona",
          "cylindrical")) {
    return false;
  }
  auto first = hm::stitching::lock_preflight_stitching_artifacts(dir.string());
  if (!first.ok() || !first->artifact_lock)
    return false;
  hm::stitching::ValidatedStitchingArtifacts previous{
      .canvas_size = first->canvas_size,
      .generation_id = first->generation_id,
      .artifact_revision = first->artifact_revision,
      .content_validated = first->content_validated,
  };
  first->artifact_lock.reset();

  config["stitching"]["projection"] = "general-panini";
  if (!write_text_file(dir / "config.yaml", YAML::Dump(config) + "\n"))
    return false;
  auto projection_changed = hm::stitching::lock_preflight_stitching_artifacts(dir.string(), 0, previous);
  if (!projection_changed.ok() || projection_changed->artifact_lock ||
      !expect_configured(dir, false, "a direct YAML projection change must invalidate existing maps")) {
    return false;
  }

  config["stitching"]["projection_parameters"]["general-panini"].push_back(100);
  config["stitching"]["projection_parameters"]["general-panini"].push_back(0);
  config["stitching"]["projection_parameters"]["general-panini"].push_back(0);
  if (!write_text_file(dir / "config.yaml", YAML::Dump(config) + "\n") ||
      !write_canvas_provenance(
          dir,
          /*max_output_width=*/0,
          /*width=*/160,
          /*height=*/32,
          /*source_width=*/0,
          /*source_height=*/0,
          /*max_canvas_dimension=*/0,
          /*max_output_width_applied=*/false,
          /*max_canvas_dimension_applied=*/false,
          "nona",
          "general-panini",
          "100,0,0")) {
    return false;
  }
  auto parameter_baseline = hm::stitching::lock_preflight_stitching_artifacts(dir.string());
  if (!parameter_baseline.ok() || !parameter_baseline->artifact_lock)
    return false;
  parameter_baseline->artifact_lock.reset();
  config["stitching"]["projection_framing"]["auto_crop"] = true;
  if (!write_text_file(dir / "config.yaml", YAML::Dump(config) + "\n") ||
      !expect_configured(dir, false, "a direct projection framing change must invalidate existing maps")) {
    return false;
  }
  config["stitching"]["projection_framing"]["auto_crop"] = false;
  config["stitching"]["projection_parameters"]["general-panini"][0] = 120;
  if (!write_text_file(dir / "config.yaml", YAML::Dump(config) + "\n") ||
      !expect_configured(dir, false, "a direct General Panini parameter change must invalidate existing maps")) {
    return false;
  }

  config["stitching"]["mapping_backend"] = "opencv-magsac";
  config["stitching"]["projection"] = "rectilinear";
  if (!write_text_file(dir / "config.yaml", YAML::Dump(config) + "\n"))
    return false;
  return expect_configured(dir, false, "a direct YAML mapping backend change must invalidate existing maps");
}

bool expect_akaze_profile_changes_require_regeneration(const fs::path& tmpdir) {
  constexpr const char* kProfileA = R"({
    "left_uniforms":{"width":7680,"height":4320,"fx":4975.75,"fy":4983.25,"cx":3824.5,"cy":2173.5,"d":[0.217,0.103,0.205,0.112]},
    "right_uniforms":{"width":7680,"height":4320,"fx":4975.75,"fy":4983.25,"cx":3824.5,"cy":2173.5,"d":[0.217,0.103,0.205,0.112]}
  })";
  constexpr const char* kProfileB = R"({
    "left_uniforms":{"width":7680,"height":4320,"fx":4976.75,"fy":4983.25,"cx":3824.5,"cy":2173.5,"d":[0.217,0.103,0.205,0.112]},
    "right_uniforms":{"width":7680,"height":4320,"fx":4975.75,"fy":4983.25,"cx":3824.5,"cy":2173.5,"d":[0.217,0.103,0.205,0.112]}
  })";
  YAML::Node config;
  config["stitching"]["control_point_matcher"] = "akaze-hamming";
  config["stitching"]["mapping_backend"] = "opencv-magsac";
  config["stitching"]["projection"] = "rectilinear";

  const auto write_akaze_provenance = [&](const fs::path& dir, const std::string& fingerprint) {
    return write_canvas_provenance(
        dir,
        /*max_output_width=*/0,
        /*width=*/160,
        /*height=*/32,
        /*source_width=*/0,
        /*source_height=*/0,
        /*max_canvas_dimension=*/0,
        /*max_output_width_applied=*/false,
        /*max_canvas_dimension_applied=*/false,
        "opencv-magsac",
        "rectilinear",
        "none",
        {},
        "akaze-hamming",
        fingerprint);
  };

  const fs::path added = tmpdir / "akaze-profile-added";
  if (!write_valid_stitching_artifacts(added) || !write_text_file(added / "config.yaml", YAML::Dump(config) + "\n") ||
      !write_akaze_provenance(added, "absent") ||
      !expect_configured(added, true, "AKAZE artifacts without an optional profile must remain usable") ||
      !write_text_file(added / "left_calibration.json", kProfileA) ||
      !expect_configured(added, false, "adding an AKAZE lens profile must invalidate existing maps")) {
    return false;
  }

  const fs::path changed = tmpdir / "akaze-profile-changed";
  if (!write_valid_stitching_artifacts(changed) ||
      !write_text_file(changed / "config.yaml", YAML::Dump(config) + "\n") ||
      !write_text_file(changed / "left_calibration.json", kProfileA)) {
    return false;
  }
  const auto loaded = hm::stitching::load_akaze_matching_calibration(changed);
  if (!loaded.ok() || !loaded->source_profile_fingerprint.has_value() ||
      !write_akaze_provenance(changed, "sha256:" + *loaded->source_profile_fingerprint) ||
      !expect_configured(changed, true, "matching AKAZE profile provenance must be reusable") ||
      !write_text_file(changed / "left_calibration.json", kProfileB) ||
      !expect_configured(changed, false, "editing an AKAZE lens profile must invalidate existing maps")) {
    return false;
  }

  const fs::path removed = tmpdir / "akaze-profile-removed";
  if (!write_valid_stitching_artifacts(removed) ||
      !write_text_file(removed / "config.yaml", YAML::Dump(config) + "\n") ||
      !write_text_file(removed / "left_calibration.json", kProfileA)) {
    return false;
  }
  const auto removed_loaded = hm::stitching::load_akaze_matching_calibration(removed);
  if (!removed_loaded.ok() || !removed_loaded->source_profile_fingerprint.has_value() ||
      !write_akaze_provenance(removed, "sha256:" + *removed_loaded->source_profile_fingerprint) ||
      !expect_configured(removed, true, "matching AKAZE profile provenance must be reusable before removal")) {
    return false;
  }
  fs::remove(removed / "left_calibration.json");
  return expect_configured(removed, false, "removing an AKAZE lens profile must invalidate existing maps");
}

bool expect_backend_choice_reader_preserves_document() {
  YAML::Node config(YAML::NodeType::Map);
  config["stitching"]["control_point_matcher"] = "superpoint-lightglue";
  config["stitching"]["mapping_backend"] = "nona";
  config["stitching"]["projection"] = "general-panini";
  config["stitching"]["run_autooptimizer"] = true;
  config["stitching"]["projection_parameters"]["general-panini"].push_back(100);
  config["stitching"]["projection_parameters"]["general-panini"].push_back(0);
  config["stitching"]["projection_parameters"]["general-panini"].push_back(0);
  config["unrelated"]["preserved"] = "value";
  const std::string before = YAML::Dump(config);
  auto choices = hm::stitching::read_stitching_backend_choices(config);
  const bool matches = choices.ok() && choices->control_point_matcher == "superpoint-lightglue" &&
      choices->mapping_backend == "nona" && choices->projection == "general-panini" && choices->run_autooptimizer &&
      choices->projection_parameters == std::vector<double>({100.0, 0.0, 0.0});
  if (!matches || YAML::Dump(config) != before) {
    std::cerr << "backend choice reader must resolve the complete tuple without mutating its YAML document: "
              << choices.status() << std::endl;
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
  stitched_output_generation: stale-generation
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
  stitched_output_generation: stale-control-point-generation
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

  const fs::path oversized_file_dir = tmpdir / "oversized_legacy_tiff_file";
  fs::remove_all(oversized_file_dir);
  fs::create_directories(oversized_file_dir);
  if (!write_mapping_tiff(oversized_file_dir / "mapping_0000.tif", 64, 32, 0.0f, 0.0f) ||
      !write_mapping_tiff(oversized_file_dir / "mapping_0001.tif", 64, 32, 32.0f, 0.0f) ||
      ::truncate((oversized_file_dir / "mapping_0000.tif").c_str(), 1024LL * 1024LL * 1024LL + 1) != 0) {
    return false;
  }
  setenv("HM_ALLOW_HARD_SEAM_FALLBACK", "1", /*overwrite=*/1);
  const auto oversized_file_status = hm::stitching::maybe_create_default_seam_file(oversized_file_dir.string());
  unsetenv("HM_ALLOW_HARD_SEAM_FALLBACK");
  if (!absl::IsResourceExhausted(oversized_file_status) || fs::exists(oversized_file_dir / "seam_file.png")) {
    std::cerr << "oversized legacy TIFF file must fail bounded preflight before parser access: "
              << oversized_file_status << std::endl;
    return false;
  }
  return true;
}

bool expect_canvas_constraint_checks_reject_oversized_artifacts(const fs::path& tmpdir) {
  const auto rejects = [](const fs::path& dir) {
    auto lock = hm::stitching::lock_canvas_constraint_artifacts(dir);
    if (!lock.ok())
      return false;
    const auto metadata = hm::stitching::check_canvas_constraint_metadata_locked(dir, /*max_output_width=*/0);
    const auto full = hm::stitching::check_canvas_constraint_locked(dir, /*max_output_width=*/0);
    return metadata.ok() && !metadata->artifacts_compatible && metadata->requires_regeneration && full.ok() &&
        !full->artifacts_compatible && full->requires_regeneration;
  };

  const fs::path oversized_tiff = tmpdir / "oversized_canvas_check_tiff";
  fs::remove_all(oversized_tiff);
  if (!write_valid_stitching_artifacts(oversized_tiff) ||
      ::truncate((oversized_tiff / "mapping_0000.tif").c_str(), 1024LL * 1024LL * 1024LL + 1) != 0 ||
      !rejects(oversized_tiff)) {
    std::cerr << "canvas compatibility checks must reject oversized TIFFs before parser access" << std::endl;
    return false;
  }

  const fs::path oversized_seam = tmpdir / "oversized_canvas_check_seam";
  fs::remove_all(oversized_seam);
  if (!write_valid_stitching_artifacts(oversized_seam) ||
      ::truncate((oversized_seam / "seam_file.png").c_str(), 512LL * 1024LL * 1024LL + 1) != 0 ||
      !rejects(oversized_seam)) {
    std::cerr << "canvas compatibility checks must reject oversized seams before parser access" << std::endl;
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

  auto preflight = hm::stitching::lock_preflight_stitching_artifacts(dir.string());
  const cv::Mat after_preflight = cv::imread((dir / "seam_file.png").string(), cv::IMREAD_GRAYSCALE);
  if (!preflight.ok() || !preflight->artifact_lock || after_preflight.size() != seam.size()) {
    std::cerr << "artifact preflight must validate cropped seam layout without normalizing its payload: "
              << preflight.status() << std::endl;
    return false;
  }
  preflight->artifact_lock.reset();

  auto artifacts = hm::stitching::lock_validated_stitching_artifacts(dir.string());
  const cv::Mat normalized = cv::imread((dir / "seam_file.png").string(), cv::IMREAD_GRAYSCALE);
  if (!artifacts.ok() || !artifacts->artifact_lock || artifacts->canvas_size.width != 160 ||
      artifacts->canvas_size.height != 32 || artifacts->load_snapshot || normalized.size() != cv::Size(160, 32)) {
    std::cerr << "runtime artifact validation must normalize a valid cropped seam: " << artifacts.status() << std::endl;
    return false;
  }
  artifacts->artifact_lock.reset();

  const fs::path stale_snapshot = dir / ".hstream-control-mask-snapshot-stale";
  fs::create_directory(stale_snapshot);
  std::ofstream(stale_snapshot / "mapping_0000.tif", std::ios::binary) << "stale";
  std::ofstream(stale_snapshot / "left.png", std::ios::binary) << "stale validation input";
  auto load = hm::stitching::lock_stitching_artifacts_for_load(dir.string());
  if (!load.ok() || !load->artifact_lock || !load->load_snapshot || fs::exists(stale_snapshot) ||
      !fs::is_regular_file(load->load_snapshot->directory() / "mapping_0000_x.tif") ||
      !load->load_snapshot->verify().ok()) {
    std::cerr << "loader validation must retain a private stable artifact snapshot: " << load.status() << std::endl;
    return false;
  }
  const fs::path pinned_mapping = dir / "mapping_0000_x.tif";
  const fs::path replacement_mapping = dir / "replacement-mapping-after-pin.tif";
  std::error_code replacement_error;
  fs::copy_file(pinned_mapping, replacement_mapping, fs::copy_options::overwrite_existing, replacement_error);
  if (!replacement_error)
    fs::remove(pinned_mapping, replacement_error);
  if (!replacement_error)
    fs::create_symlink(replacement_mapping, pinned_mapping, replacement_error);
  if (replacement_error || load->load_snapshot->verify().ok()) {
    std::cerr << "loader validation must reject a control-mask path replaced after descriptor pinning" << std::endl;
    return false;
  }
  const fs::path snapshot_directory = load->load_snapshot->directory();
  load->load_snapshot.reset();
  if (fs::exists(snapshot_directory)) {
    std::cerr << "stable artifact snapshot must be removed when its loader releases it" << std::endl;
    return false;
  }
  return true;
}

bool expect_preflight_snapshot_is_reused_only_for_same_generation(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "validated_snapshot_generation";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir)) {
    return false;
  }

  auto first = hm::stitching::lock_preflight_stitching_artifacts(dir.string());
  if (!first.ok() || !first->artifact_lock) {
    std::cerr << "validated snapshot fixture must pass initial validation: " << first.status() << std::endl;
    return false;
  }
  const hm::stitching::ValidatedStitchingArtifacts snapshot{
      .canvas_size = first->canvas_size,
      .generation_id = first->generation_id,
      .artifact_revision = first->artifact_revision,
      .content_validated = first->content_validated,
      .max_output_width = 0,
      .max_canvas_dimension = hm::stitching::live_stitch_max_canvas_dimension(),
  };
  first->artifact_lock.reset();

  auto reused = hm::stitching::lock_preflight_stitching_artifacts(dir.string(), 0, snapshot);
  const bool same_generation_reused = reused.ok() && reused->artifact_lock &&
      reused->generation_id == snapshot.generation_id && reused->canvas_size.width == snapshot.canvas_size.width &&
      reused->canvas_size.height == snapshot.canvas_size.height;
  if (reused.ok())
    reused->artifact_lock.reset();

  hm::stitching::ValidatedStitchingArtifacts unreliable_snapshot = snapshot;
  ++unreliable_snapshot.canvas_size.width;
  setenv("HM_TEST_STITCH_UNRELIABLE_METADATA", "1", 1);
  auto unreliable = hm::stitching::lock_preflight_stitching_artifacts(dir.string(), 0, unreliable_snapshot);
  unsetenv("HM_TEST_STITCH_UNRELIABLE_METADATA");
  const bool unreliable_snapshot_rejected = unreliable.ok() && unreliable->artifact_lock &&
      unreliable->canvas_size.width == first->canvas_size.width &&
      unreliable->canvas_size.height == first->canvas_size.height;
  if (unreliable.ok())
    unreliable->artifact_lock.reset();

  set_write_time(dir / "left.png", 4);
  auto stale_inputs = hm::stitching::lock_preflight_stitching_artifacts(dir.string(), 0, snapshot);
  const bool stale_inputs_rejected = stale_inputs.ok() && !stale_inputs->artifact_lock;
  set_write_time(dir / "left.png", 0);

  cv::Mat seam(32, 160, CV_8U, cv::Scalar(255));
  const fs::path replacement = dir / "replacement_seam.png";
  if (!cv::imwrite(replacement.string(), seam)) {
    return false;
  }
  fs::rename(replacement, dir / "seam_file.png");
  auto replaced = hm::stitching::lock_preflight_stitching_artifacts(dir.string(), 0, snapshot);
  const bool replacement_revalidated = replaced.ok() && replaced->artifact_lock &&
      replaced->generation_id != snapshot.generation_id && replaced->artifact_revision != snapshot.artifact_revision &&
      !replaced->content_validated;
  if (replaced.ok())
    replaced->artifact_lock.reset();
  if (!same_generation_reused || !unreliable_snapshot_rejected || !stale_inputs_rejected || !replacement_revalidated) {
    std::cerr << "validated snapshots must be reused only while the exact artifact generation and dependencies are "
                 "unchanged: "
              << replaced.status() << std::endl;
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

bool expect_malformed_live_limit_is_ignored() {
  unsetenv("HM_ALLOW_OVERSIZED_LIVE_STITCH");
  setenv("HM_MAX_LIVE_STITCH_EGL_DIMENSION", "4096junk", /*overwrite=*/1);
  const auto effective_limit = hm::stitching::live_stitch_max_canvas_dimension();
  unsetenv("HM_MAX_LIVE_STITCH_EGL_DIMENSION");
  if (effective_limit.has_value() && *effective_limit == 4096) {
    std::cerr << "a malformed live canvas limit must not be parsed as a valid numeric prefix" << std::endl;
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
  auto preflight = hm::stitching::lock_preflight_stitching_artifacts(dir.string());
  const bool preflight_accepts_layout = preflight.ok() && preflight->artifact_lock;
  if (preflight.ok())
    preflight->artifact_lock.reset();
  auto authoritative = hm::stitching::lock_validated_stitching_artifacts(dir.string());
  const bool authoritative_rejects_content = !authoritative.ok() || !authoritative->artifact_lock;
  if (authoritative.ok())
    authoritative->artifact_lock.reset();
  auto lightweight = hm::stitching::try_lock_canvas_constraint_check(dir, /*max_output_width=*/0);
  const bool lightweight_rejects = lightweight.ok() && !lightweight->artifacts_compatible &&
      lightweight->requires_regeneration && lightweight->artifact_lock;
  if (lightweight.ok())
    lightweight->artifact_lock.reset();
  auto metadata_lock = hm::stitching::try_lock_canvas_constraint_artifacts(dir);
  const auto metadata = metadata_lock.ok() && *metadata_lock
      ? hm::stitching::check_canvas_constraint_metadata_locked(dir, /*max_output_width=*/0)
      : absl::StatusOr<hm::stitching::CanvasConstraintCompatibility>(
            metadata_lock.ok() ? absl::UnavailableError("artifact lock unavailable") : metadata_lock.status());
  const bool metadata_accepts_layout =
      metadata.ok() && metadata->artifacts_compatible && !metadata->requires_regeneration;
  if (metadata_lock.ok())
    metadata_lock->reset();
  if (!configured.ok() || *configured || !preflight_accepts_layout || !authoritative_rejects_content ||
      !lightweight_rejects || !metadata_accepts_layout) {
    std::cerr << "uniform same-size seam must be deferred by metadata preflight and rejected by authoritative checks: "
              << configured.status() << " / " << metadata.status() << std::endl;
    return false;
  }
  return true;
}

bool expect_corrupt_idat_is_rejected_without_crashing(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "corrupt_idat_canvas_check";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir) || !corrupt_png_idat_with_valid_crc(dir / "seam_file.png"))
    return false;

  auto preflight = hm::stitching::lock_preflight_stitching_artifacts(dir.string());
  const bool preflight_accepts_layout = preflight.ok() && preflight->artifact_lock;
  if (preflight.ok())
    preflight->artifact_lock.reset();
  auto authoritative = hm::stitching::lock_validated_stitching_artifacts(dir.string());
  const bool authoritative_rejects_content = !authoritative.ok() || !authoritative->artifact_lock;
  if (authoritative.ok())
    authoritative->artifact_lock.reset();
  auto lightweight = hm::stitching::try_lock_canvas_constraint_check(dir, /*max_output_width=*/0);
  const bool rejected = lightweight.ok() && !lightweight->artifacts_compatible && lightweight->requires_regeneration &&
      lightweight->artifact_lock;
  if (lightweight.ok())
    lightweight->artifact_lock.reset();
  if (!preflight_accepts_layout || !authoritative_rejects_content || !rejected) {
    std::cerr
        << "corrupt PNG IDAT must be deferred by preflight and rejected by authoritative checks without crashing: "
        << lightweight.status() << std::endl;
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
  hugin_lock->reset();

  auto stale_surface_generation = hm::stitching::stitched_output_generation_id(*first_hugin_generation, 1.0, 160, 32);
  NvBufSurfaceParams stale_surface_params{};
  stale_surface_params.width = 160;
  stale_surface_params.height = 32;
  stale_surface_params.pitch = 160 * 4;
  stale_surface_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  hm::surface::Surface stale_surface(&stale_surface_params);
  const auto stale_surface_status = stale_surface_generation.ok()
      ? hm::stitching::save_stitched_image(dir.string(), stale_surface, *stale_surface_generation)
      : stale_surface_generation.status();
  if (!absl::IsAborted(stale_surface_status)) {
    std::cerr << "a stale surface must be rejected before GPU readback: " << stale_surface_status << std::endl;
    return false;
  }
  hugin_lock = hm::stitching::HuginProject::RecoverAndLock(dir);
  if (!hugin_lock.ok())
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

bool expect_validated_load_snapshot_rejects_path_replacement(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "validated_load_snapshot_replacement";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir))
    return false;
  const fs::path mapping = dir / "mapping_0000_x.tif";
  const fs::path replacement = dir / "replacement-mapping.tif";
  std::error_code error;
  fs::copy_file(mapping, replacement, fs::copy_options::overwrite_existing, error);
  if (error)
    return false;

  absl::StatusOr<hm::stitching::LockedStitchingArtifacts> result = absl::UnknownError("not started");
  const fs::path marker = dir / ".load-snapshot-ready";
  const fs::path release = dir / ".load-snapshot-release";
  ::setenv("HM_TEST_STITCH_LOAD_SNAPSHOT_DELAY_MS", "3000", 1);
  ::setenv("HM_TEST_STITCH_LOAD_SNAPSHOT_MARKER", marker.c_str(), 1);
  ::setenv("HM_TEST_STITCH_LOAD_SNAPSHOT_RELEASE", release.c_str(), 1);
  std::thread loader([&] { result = hm::stitching::lock_stitching_artifacts_for_load(dir.string()); });
  const bool snapshot_started = wait_for_test_marker(marker);
  if (snapshot_started) {
    fs::remove(mapping, error);
    if (!error)
      fs::create_symlink(replacement, mapping, error);
  }
  std::ofstream(release) << "continue\n";
  loader.join();
  ::unsetenv("HM_TEST_STITCH_LOAD_SNAPSHOT_RELEASE");
  ::unsetenv("HM_TEST_STITCH_LOAD_SNAPSHOT_MARKER");
  ::unsetenv("HM_TEST_STITCH_LOAD_SNAPSHOT_DELAY_MS");

  bool snapshot_left_behind = false;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_directory() && entry.path().filename().string().rfind(".hstream-control-mask-snapshot-", 0) == 0)
      snapshot_left_behind = true;
  }
  if (!snapshot_started || error || result.ok() || !fs::is_symlink(fs::symlink_status(mapping)) ||
      snapshot_left_behind) {
    std::cerr << "validated control-mask loading must reject path replacement without leaking its private snapshot: "
              << result.status() << std::endl;
    return false;
  }
  return true;
}

bool expect_validation_rejects_pre_generation_replacement(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "validated_generation_replacement";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir))
    return false;
  const fs::path mapping = dir / "mapping_0000_x.tif";
  const fs::path replacement = dir / "replacement-mapping.tif";
  if (!write_remap_tiff(replacement, 8, 8))
    return false;

  absl::StatusOr<hm::stitching::LockedStitchingArtifacts> result = absl::UnknownError("not started");
  const fs::path marker = dir / ".post-validation-ready";
  const fs::path release = dir / ".post-validation-release";
  ::setenv("HM_TEST_STITCH_POST_VALIDATION_DELAY_MS", "3000", 1);
  ::setenv("HM_TEST_STITCH_POST_VALIDATION_MARKER", marker.c_str(), 1);
  ::setenv("HM_TEST_STITCH_POST_VALIDATION_RELEASE", release.c_str(), 1);
  std::thread loader([&] { result = hm::stitching::lock_stitching_artifacts_for_load(dir.string()); });
  const bool reached_delay = wait_for_test_marker(marker);
  std::error_code error;
  if (reached_delay)
    fs::rename(replacement, mapping, error);
  std::ofstream(release) << "continue\n";
  loader.join();
  ::unsetenv("HM_TEST_STITCH_POST_VALIDATION_RELEASE");
  ::unsetenv("HM_TEST_STITCH_POST_VALIDATION_MARKER");
  ::unsetenv("HM_TEST_STITCH_POST_VALIDATION_DELAY_MS");

  if (!reached_delay || error || !absl::IsAborted(result.status())) {
    std::cerr << "validated control-mask loading must reject path replacement before generation capture: "
              << result.status() << std::endl;
    return false;
  }
  return true;
}

bool expect_unreliable_validation_uses_pinned_generation(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "unreliable_validation_pinned_generation";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir))
    return false;
  auto adopted = hm::stitching::lock_validated_stitching_artifacts(dir.string());
  if (!adopted.ok() || !adopted->artifact_lock)
    return false;
  adopted->artifact_lock.reset();
  std::ifstream expected_identity_input(dir / hm::stitching::kStitchGenerationArtifact, std::ios::binary);
  const std::string expected_identity{
      std::istreambuf_iterator<char>(expected_identity_input), std::istreambuf_iterator<char>()};
  if (expected_identity.empty())
    return false;
  const fs::path mapping = dir / "mapping_0001.tif";
  const fs::path original = dir / "mapping_0001-original.tif";
  const fs::path replacement = dir / "mapping_0001-replacement.tif";
  if (!write_mapping_tiff(replacement, 64, 32, 200.0f, 0.0f))
    return false;

  absl::StatusOr<hm::stitching::LockedStitchingArtifacts> result = absl::UnknownError("not started");
  const fs::path marker = dir / ".stable-validation-ready";
  const fs::path release = dir / ".stable-validation-release";
  ::setenv("HM_TEST_STITCH_UNRELIABLE_METADATA", "1", 1);
  ::setenv("HM_TEST_STITCH_DISABLE_VALIDATION_CLONE", "1", 1);
  ::setenv("HM_TEST_STITCH_STABLE_VALIDATION_DELAY_MS", "3000", 1);
  ::setenv("HM_TEST_STITCH_STABLE_VALIDATION_MARKER", marker.c_str(), 1);
  ::setenv("HM_TEST_STITCH_STABLE_VALIDATION_RELEASE", release.c_str(), 1);
  std::thread loader([&] { result = hm::stitching::lock_stitching_artifacts_for_load(dir.string()); });
  const bool reached_delay = wait_for_test_marker(marker);
  std::error_code error;
  if (reached_delay)
    fs::rename(mapping, original, error);
  if (reached_delay && !error)
    fs::rename(replacement, mapping, error);
  if (reached_delay && !error)
    fs::rename(mapping, replacement, error);
  if (reached_delay && !error)
    fs::rename(original, mapping, error);
  std::ofstream(release) << "continue\n";
  loader.join();
  ::unsetenv("HM_TEST_STITCH_STABLE_VALIDATION_RELEASE");
  ::unsetenv("HM_TEST_STITCH_STABLE_VALIDATION_MARKER");
  ::unsetenv("HM_TEST_STITCH_STABLE_VALIDATION_DELAY_MS");
  ::unsetenv("HM_TEST_STITCH_DISABLE_VALIDATION_CLONE");

  std::ifstream snapshot_identity_input(
      result.ok() && result->load_snapshot
          ? result->load_snapshot->directory() / hm::stitching::kStitchGenerationArtifact
          : fs::path(),
      std::ios::binary);
  const std::string snapshot_identity{
      std::istreambuf_iterator<char>(snapshot_identity_input), std::istreambuf_iterator<char>()};
  const bool transient_verified = result.ok() && result->load_snapshot && result->load_snapshot->verify().ok();
  const fs::path persistently_changed = dir / "mapping_0001_x.tif";
  const auto preserved_write_time = fs::last_write_time(persistently_changed);
  std::fstream changed(persistently_changed, std::ios::binary | std::ios::in | std::ios::out);
  changed.seekg(0, std::ios::end);
  const std::streamoff changed_size = changed.tellg();
  char changed_byte = 0;
  if (changed_size > 0) {
    changed.seekg(changed_size / 2);
    changed.get(changed_byte);
    changed_byte ^= 0x5a;
    changed.seekp(changed_size / 2);
    changed.put(changed_byte);
    changed.flush();
  }
  changed.close();
  fs::last_write_time(persistently_changed, preserved_write_time);
  const bool persistent_change_rejected = result.ok() && result->load_snapshot && !result->load_snapshot->verify().ok();
  ::unsetenv("HM_TEST_STITCH_UNRELIABLE_METADATA");

  if (!reached_delay || error || !transient_verified || changed_size <= 0 || !persistent_change_rejected ||
      !result.ok() || !result->artifact_lock || !result->load_snapshot || result->canvas_size.width != 160 ||
      result->canvas_size.height != 32 || fs::is_symlink(result->load_snapshot->directory() / "mapping_0001.tif") ||
      snapshot_identity != expected_identity) {
    std::cerr << "unreliable-filesystem validation must use one pinned artifact generation: " << result.status()
              << std::endl;
    return false;
  }
  return true;
}

bool expect_validation_copy_offload_outcomes(const fs::path& tmpdir) {
  for (const char* outcome : {"unsupported", "partial", "zero"}) {
    const fs::path dir = tmpdir / ("validation_copy_offload_" + std::string(outcome));
    fs::remove_all(dir);
    if (!write_valid_stitching_artifacts(dir))
      return false;
    ::setenv("HM_TEST_STITCH_UNRELIABLE_METADATA", "1", 1);
    ::setenv("HM_TEST_STITCH_DISABLE_VALIDATION_CLONE", "1", 1);
    ::setenv("HM_TEST_STITCH_COPY_FILE_RANGE_RESULT", outcome, 1);
    auto load = hm::stitching::lock_stitching_artifacts_for_load(dir.string());
    const bool verified = load.ok() && load->artifact_lock && load->load_snapshot && load->load_snapshot->verify().ok();
    ::unsetenv("HM_TEST_STITCH_COPY_FILE_RANGE_RESULT");
    ::unsetenv("HM_TEST_STITCH_DISABLE_VALIDATION_CLONE");
    ::unsetenv("HM_TEST_STITCH_UNRELIABLE_METADATA");
    if (!verified) {
      std::cerr << "validation snapshot copy outcome must remain loadable (" << outcome << "): " << load.status()
                << std::endl;
      return false;
    }
  }
  return true;
}

bool expect_unreliable_load_refreshes_legacy_identity_revision(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "unreliable_legacy_identity_revision";
  fs::remove_all(dir);
  if (!write_valid_stitching_artifacts(dir))
    return false;
  auto artifact_lock = hm::stitching::HuginProject::RecoverAndLock(dir);
  if (!artifact_lock.ok())
    return false;
  auto bindings = hm::stitching::stitch_artifact_binding_revision_locked(dir);
  if (!bindings.ok())
    return false;
  const std::string logical_id = "legacy-v2-logical-generation";
  std::ofstream identity(dir / hm::stitching::kStitchGenerationArtifact, std::ios::binary | std::ios::trunc);
  identity << "version=2\nlogical-size=" << logical_id.size() << "\nbindings-size=" << bindings->size() << '\n'
           << logical_id << *bindings;
  identity.close();
  artifact_lock->reset();
  if (!identity)
    return false;

  ::setenv("HM_TEST_STITCH_UNRELIABLE_METADATA", "1", 1);
  auto load = hm::stitching::lock_stitching_artifacts_for_load(dir.string());
  const bool verified = load.ok() && load->artifact_lock && load->load_snapshot && load->generation_id == logical_id &&
      load->load_snapshot->verify().ok();
  ::unsetenv("HM_TEST_STITCH_UNRELIABLE_METADATA");
  if (!verified) {
    std::cerr << "unreliable legacy identity migration must refresh the load revision: " << load.status() << std::endl;
    return false;
  }
  return true;
}

bool expect_akaze_calibration_loading_contract(const fs::path& tmpdir) {
  const fs::path missing = tmpdir / "missing-akaze-calibration";
  fs::create_directories(missing);
  const auto absent = hm::stitching::load_akaze_matching_calibration(missing);
  if (!absent.ok() || absent->left.has_value() || absent->right.has_value()) {
    std::cerr << "missing AKAZE calibration must remain optional: " << absent.status() << std::endl;
    return false;
  }

  const fs::path malformed = tmpdir / "malformed-akaze-calibration";
  fs::create_directories(malformed);
  if (!write_text_file(malformed / "left_calibration.json", "{not-valid-json"))
    return false;
  const auto invalid = hm::stitching::load_akaze_matching_calibration(malformed);
  if (!absl::IsInvalidArgument(invalid.status())) {
    std::cerr << "malformed AKAZE calibration must fail closed: " << invalid.status() << std::endl;
    return false;
  }

  const fs::path valid = tmpdir / "valid-akaze-calibration";
  fs::create_directories(valid);
  constexpr const char* kCamera = R"({
    "width": 7680, "height": 4320,
    "fx": 4975.75, "fy": 4983.25, "cx": 3824.5, "cy": 2173.5,
    "d": [0.217, 0.103, 0.205, 0.112]
  })";
  if (!write_text_file(
          valid / "left_calibration.json",
          std::string("{\"left_uniforms\":") + kCamera + ",\"right_uniforms\":" + kCamera + "}")) {
    return false;
  }
  const auto loaded = hm::stitching::load_akaze_matching_calibration(valid);
  if (!loaded.ok() || !loaded->left.has_value() || !loaded->right.has_value() ||
      !loaded->source_profile_fingerprint.has_value() || loaded->source_profile_fingerprint->size() != 64 ||
      loaded->left->resolution != cv::Size(7680, 4320) || std::abs(loaded->right->fy - 4983.25) > 1e-9) {
    std::cerr << "valid paired AKAZE calibration was not loaded: " << loaded.status() << std::endl;
    return false;
  }
  const fs::path isolated = tmpdir / "symlinked-akaze-calibration";
  fs::create_directories(isolated);
  std::error_code symlink_error;
  fs::create_symlink(valid / "left_calibration.json", isolated / "left_calibration.json", symlink_error);
  const auto symlinked = symlink_error ? decltype(loaded)(absl::InternalError(symlink_error.message()))
                                       : hm::stitching::load_akaze_matching_calibration(isolated);
  if (!symlinked.ok() || !symlinked->left.has_value() ||
      symlinked->source_profile_fingerprint != loaded->source_profile_fingerprint) {
    std::cerr << "isolated games must load and fingerprint their symlinked AKAZE profile: " << symlinked.status()
              << std::endl;
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
  ::setenv("HM_TEST_FORCE_TRANSACTION_RECOVERY_SCAN", "1", 1);
  const fs::path tmpdir =
      fs::temp_directory_path() / ("configure_stitching_canvas_cap_test_" + std::to_string(::getpid()));
  fs::remove_all(tmpdir);
  fs::create_directories(tmpdir);
  if (!expect_akaze_calibration_loading_contract(tmpdir)) {
    finish(tmpdir, 47);
  }
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

  if (!expect_mapping_algorithm_changes_require_regeneration(tmpdir)) {
    finish(tmpdir, 48);
  }

  if (!expect_akaze_profile_changes_require_regeneration(tmpdir)) {
    finish(tmpdir, 50);
  }

  if (!expect_backend_choice_reader_preserves_document()) {
    finish(tmpdir, 49);
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

  if (!expect_canvas_constraint_checks_reject_oversized_artifacts(tmpdir)) {
    finish(tmpdir, 41);
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

  if (!expect_preflight_snapshot_is_reused_only_for_same_generation(tmpdir)) {
    finish(tmpdir, 36);
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

  if (!expect_malformed_live_limit_is_ignored()) {
    finish(tmpdir, 34);
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

  if (!expect_corrupt_idat_is_rejected_without_crashing(tmpdir)) {
    finish(tmpdir, 35);
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

  if (!expect_validated_load_snapshot_rejects_path_replacement(tmpdir)) {
    finish(tmpdir, 42);
  }

  if (!expect_validation_rejects_pre_generation_replacement(tmpdir)) {
    finish(tmpdir, 43);
  }

  if (!expect_unreliable_validation_uses_pinned_generation(tmpdir)) {
    finish(tmpdir, 44);
  }

  if (!expect_unreliable_load_refreshes_legacy_identity_revision(tmpdir)) {
    finish(tmpdir, 45);
  }

  if (!expect_validation_copy_offload_outcomes(tmpdir)) {
    finish(tmpdir, 46);
  }

  finish(tmpdir, 0);
}
