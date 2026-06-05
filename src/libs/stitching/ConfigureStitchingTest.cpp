#include "hstream/src/libs/stitching/ConfigureStitching.h"

#include <tiffio.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
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

bool run_scoreboard_selector_env_child(const fs::path& tmpdir) {
  const pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "scoreboard selector env: fork failed" << std::endl;
    return false;
  }
  if (pid == 0) {
    const fs::path game_root = tmpdir / "scoreboard_games";
    const fs::path game_dir = game_root / "scoreboard-game";
    const fs::path bin_dir = tmpdir / "fake-bin";
    fs::create_directories(game_dir);
    fs::create_directories(bin_dir);
    unsetenv("HMLIB_ROOT");
    unsetenv("HM_ROOT");
    if (chdir(tmpdir.c_str()) != 0) {
      std::cerr << "scoreboard selector env: chdir failed" << std::endl;
      _exit(5);
    }

    const fs::path hmscoreboard = bin_dir / "hmscoreboard";
    if (!write_text_file(
            hmscoreboard,
            R"(#!/bin/sh
set -eu
game_id=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --game-id)
      shift
      game_id="${1:-}"
      ;;
    --game-id=*)
      game_id="${1#--game-id=}"
      ;;
  esac
  shift || true
done
if [ -z "${game_id}" ]; then
  exit 64
fi
printf '%s\n' "${PYTHONUNBUFFERED:-}" > "${HM_GAME_DIR}/py_unbuffered.txt"
mkdir -p "${HM_GAME_DIR}/${game_id}"
cat > "${HM_GAME_DIR}/${game_id}/config.yaml" <<'YAML'
rink:
  scoreboard:
    perspective_polygon:
      - [0, 0]
      - [10, 0]
      - [10, 10]
      - [0, 10]
YAML
)")) {
      _exit(1);
    }
    if (chmod(hmscoreboard.c_str(), 0755) != 0) {
      std::cerr << "scoreboard selector env: chmod failed" << std::endl;
      _exit(2);
    }

    const char* old_path = getenv("PATH");
    const std::string path = bin_dir.string() + (old_path && *old_path ? ":" + std::string(old_path) : "");
    setenv("PATH", path.c_str(), /*overwrite=*/1);
    setenv("HM_GAME_DIR", game_root.c_str(), /*overwrite=*/1);

    const auto status = hm::stitching::configure_scoreboard(game_dir.string());
    if (!status.ok()) {
      std::cerr << "scoreboard selector env: configure_scoreboard failed: " << status << std::endl;
      _exit(3);
    }

    std::ifstream unbuffered_file(game_root / "py_unbuffered.txt");
    std::string value;
    std::getline(unbuffered_file, value);
    if (value != "1") {
      std::cerr << "scoreboard selector env: expected PYTHONUNBUFFERED=1, got \"" << value << '"' << std::endl;
      _exit(4);
    }
    _exit(0);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    std::cerr << "scoreboard selector env: waitpid failed" << std::endl;
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::cerr << "scoreboard selector env: child failed with status " << status << std::endl;
    return false;
  }
  return true;
}

bool run_scoreboard_selector_python_args_child(const fs::path& tmpdir) {
  const pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "scoreboard selector python args: fork failed" << std::endl;
    return false;
  }
  if (pid == 0) {
    const fs::path game_root = tmpdir / "scoreboard_python_games";
    const fs::path game_dir = game_root / "scoreboard-python-game";
    const fs::path bin_dir = tmpdir / "fake-python-bin";
    const fs::path hm_root = tmpdir / "fake-hm";
    fs::create_directories(game_dir);
    fs::create_directories(bin_dir);
    fs::create_directories(hm_root / "hmlib");

    const fs::path python = bin_dir / "python3";
    if (!write_text_file(
            python,
            R"(#!/bin/sh
set -eu
game_id=""
bind_host=""
port=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --game-id)
      shift
      game_id="${1:-}"
      ;;
    --game-id=*)
      game_id="${1#--game-id=}"
      ;;
    --selector-bind-host)
      shift
      bind_host="${1:-}"
      ;;
    --selector-bind-host=*)
      bind_host="${1#--selector-bind-host=}"
      ;;
    --selector-port)
      shift
      port="${1:-}"
      ;;
    --selector-port=*)
      port="${1#--selector-port=}"
      ;;
  esac
  shift || true
done
if [ -z "${game_id}" ] || [ -z "${bind_host}" ] || [ -z "${port}" ]; then
  exit 64
fi
{
  printf 'PYTHONUNBUFFERED=%s\n' "${PYTHONUNBUFFERED:-}"
  printf 'bind_host=%s\n' "${bind_host}"
  printf 'port=%s\n' "${port}"
} > "${HM_GAME_DIR}/selector_args.txt"
mkdir -p "${HM_GAME_DIR}/${game_id}"
cat > "${HM_GAME_DIR}/${game_id}/config.yaml" <<'YAML'
rink:
  scoreboard:
    perspective_polygon:
      - [0, 0]
      - [10, 0]
      - [10, 10]
      - [0, 10]
YAML
)")) {
      _exit(1);
    }
    if (chmod(python.c_str(), 0755) != 0) {
      std::cerr << "scoreboard selector python args: chmod failed" << std::endl;
      _exit(2);
    }

    const char* old_path = getenv("PATH");
    const std::string path = bin_dir.string() + (old_path && *old_path ? ":" + std::string(old_path) : "");
    setenv("PATH", path.c_str(), /*overwrite=*/1);
    setenv("HM_GAME_DIR", game_root.c_str(), /*overwrite=*/1);
    setenv("HMLIB_ROOT", hm_root.c_str(), /*overwrite=*/1);
    unsetenv("HM_ROOT");

    const auto status = hm::stitching::configure_scoreboard(game_dir.string());
    if (!status.ok()) {
      std::cerr << "scoreboard selector python args: configure_scoreboard failed: " << status << std::endl;
      _exit(3);
    }

    std::ifstream args_file(game_root / "selector_args.txt");
    std::string line;
    bool saw_unbuffered = false;
    bool saw_bind_host = false;
    bool saw_port = false;
    while (std::getline(args_file, line)) {
      if (line == "PYTHONUNBUFFERED=1") {
        saw_unbuffered = true;
      } else if (line == "bind_host=0.0.0.0") {
        saw_bind_host = true;
      } else if (line.rfind("port=", 0) == 0 && line.size() > 5 && line != "port=0") {
        saw_port = true;
      }
    }
    if (!saw_unbuffered || !saw_bind_host || !saw_port) {
      std::cerr << "scoreboard selector python args: missing expected selector args" << std::endl;
      _exit(4);
    }
    _exit(0);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    std::cerr << "scoreboard selector python args: waitpid failed" << std::endl;
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::cerr << "scoreboard selector python args: child failed with status " << status << std::endl;
    return false;
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

  if (!run_scoreboard_selector_python_args_child(tmpdir)) {
    finish(tmpdir, 6);
  }

  if (!expect_dependency_invalidation_report(tmpdir)) {
    finish(tmpdir, 7);
  }

  finish(tmpdir, 0);
}
