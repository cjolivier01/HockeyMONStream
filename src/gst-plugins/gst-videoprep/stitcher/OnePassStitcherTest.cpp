#include "hstream/src/gst-plugins/gst-videoprep/stitcher/stitcher.h"
#include "absl/status/status.h"

#include <gst/gst.h>

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

// Minimal integration-style check that one-pass mode publishes a fallback canvas
// size when control masks are absent.
int main() {
  gst_init(nullptr, nullptr);

  fs::path tmpdir = fs::temp_directory_path() / "hmstitcher_onepass_test";
  fs::create_directories(tmpdir);

  hm::stitcher::StitcherPriv stitcher(/*gpu_id=*/0, /*batch_size=*/2);
  stitcher.SetProperty({"one-pass-mode", "1"});

  hm::DSCustom_CreateParams params{};
  std::string config_dir = tmpdir.string();
  params.config_file = const_cast<char*>(config_dir.c_str());
  params.m_inCaps = gst_caps_from_string("video/x-raw,format=RGBA,width=1280,height=720");

  absl::Status status = stitcher.PreCapsInit(&params);
  if (!status.ok()) {
    std::cerr << "PreCapsInit failed: " << status << std::endl;
    return 1;
  }

  if (params.output_width_height[0] != 2560 || params.output_width_height[1] != 720) {
    std::cerr << "Unexpected fallback size: " << params.output_width_height[0] << "x"
              << params.output_width_height[1] << std::endl;
    return 2;
  }

  gst_caps_unref(params.m_inCaps);
  return 0;
}
