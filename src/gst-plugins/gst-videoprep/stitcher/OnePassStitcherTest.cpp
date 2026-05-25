#include "hstream/src/gst-plugins/gst-videoprep/stitcher/stitcher.h"

#include "absl/status/status.h"

#include <filesystem>
#include <iostream>
#include <string>

#include <gst/gst.h>

namespace fs = std::filesystem;

namespace {

bool run_precaps(
    const std::string& config_dir,
    bool one_pass_mode,
    bool expect_ok,
    size_t expected_width,
    size_t expected_height) {
  hm::stitcher::StitcherPriv stitcher(/*gpu_id=*/0, /*batch_size=*/2);
  if (one_pass_mode) {
    stitcher.SetProperty({"one-pass-mode", "1"});
  }

  hm::DSCustom_CreateParams params{};
  params.config_file = const_cast<char*>(config_dir.c_str());
  params.m_inCaps = gst_caps_from_string("video/x-raw,format=RGBA,width=1280,height=720");
  params.output_width_height[0] = 0;
  params.output_width_height[1] = 0;

  absl::Status status = stitcher.PreCapsInit(&params);
  if (params.m_inCaps) {
    gst_caps_unref(params.m_inCaps);
  }

  if (expect_ok != status.ok()) {
    std::cerr << "Unexpected PreCapsInit status for one_pass_mode=" << one_pass_mode << ": " << status << std::endl;
    return false;
  }

  if (!expect_ok) {
    return true;
  }

  if (params.output_width_height[0] != expected_width || params.output_width_height[1] != expected_height) {
    std::cerr << "Unexpected PreCapsInit size: " << params.output_width_height[0] << "x"
              << params.output_width_height[1] << ", expected: " << expected_width << "x" << expected_height
              << std::endl;
    return false;
  }

  return true;
}

} // namespace

int main() {
  gst_init(nullptr, nullptr);

  fs::path tmpdir = fs::temp_directory_path() / "hmstitcher_onepass_test";
  fs::remove_all(tmpdir);
  fs::create_directories(tmpdir);

  const std::string config_dir = tmpdir.string();
  if (!run_precaps(config_dir, /*one_pass_mode=*/false, /*expect_ok=*/false, 0, 0)) {
    return 1;
  }
  if (!run_precaps(config_dir, /*one_pass_mode=*/true, /*expect_ok=*/true, 0, 0)) {
    return 2;
  }

  return 0;
}
