#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"

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

bool expect_legacy_seam_generation_rejects_oversized_tiff(const fs::path& tmpdir) {
  const fs::path dir = tmpdir / "oversized_legacy_tiff";
  fs::remove_all(dir);
  fs::create_directories(dir);
  if (!write_mapping_tiff(dir / "mapping_0000.tif", 40000, 1, 0.0f, 0.0f) ||
      !write_mapping_tiff(dir / "mapping_0001.tif", 1, 1, 0.0f, 0.0f)) {
    return false;
  }
  const auto status = hm::stitching::maybe_create_default_seam_file(dir.string());
  if (!absl::IsResourceExhausted(status) || fs::exists(dir / "seam_file.png")) {
    std::cerr << "oversized legacy TIFF must fail before seam allocation: " << status << std::endl;
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

  if (!expect_clean_waits_for_transaction_locks(tmpdir)) {
    finish(tmpdir, 9);
  }

  if (!expect_legacy_seam_generation_rejects_oversized_tiff(tmpdir)) {
    finish(tmpdir, 10);
  }

  finish(tmpdir, 0);
}
