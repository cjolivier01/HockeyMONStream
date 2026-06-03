#include "hstream/src/libs/stitching/ConfigureStitching.h"

#include <tiffio.h>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
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

  for (const char* filename :
       {"mapping_0000_x.tif", "mapping_0000_y.tif", "mapping_0001_x.tif", "mapping_0001_y.tif"}) {
    if (!write_text_file(dir / filename, "placeholder")) {
      return false;
    }
  }

  set_write_time(dir / "left.png", 0);
  set_write_time(dir / "right.png", 0);
  set_write_time(dir / "hm_project.pto", 1);
  set_write_time(dir / "autooptimiser_out.pto", 2);
  for (const char* filename : {"mapping_0000.tif",
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

  finish(tmpdir, 0);
}
