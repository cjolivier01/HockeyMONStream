#include "hstream/src/gst-plugins/gst-videoprep/playtracker/playtracker.h"

#include "absl/status/status.h"

#include <cuda_runtime.h>
#include <gst/gst.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path write_minimal_config(const fs::path& dir) {
  fs::create_directories(dir);
  fs::path cfg = dir / "play_tracker_config.yaml";

  std::ofstream out(cfg);
  out << "play-tracker:\n";
  out << "  fps-speed-scale: 1.0\n";
  out << "  live-boxes:\n";
  out << "    - name: current_roi\n";
  return cfg;
}

} // namespace

int main() {
  gst_init(nullptr, nullptr);

  const fs::path tmpdir = fs::temp_directory_path() / "vpplaytracker_priv_init_test";
  fs::remove_all(tmpdir);
  const fs::path cfg = write_minimal_config(tmpdir);
  const std::string cfg_str = cfg.string();

  cudaStream_t stream = nullptr;
  const cudaError_t cuda_err = cudaStreamCreate(&stream);
  if (cuda_err != cudaSuccess || stream == nullptr) {
    std::cerr << "cudaStreamCreate failed: " << cudaGetErrorString(cuda_err) << std::endl;
    return 10;
  }

  hm::playtracker::PlayTrackerPriv priv(/*gpu_id=*/0, /*batch_size=*/1);

  hm::DSCustom_CreateParams params{};
  params.config_file = const_cast<char*>(cfg_str.c_str());
  params.m_gpuId = 0;
  params.m_cudaStream = stream;
  params.m_inCaps = gst_caps_from_string("video/x-raw,format=RGBA,width=1280,height=720,framerate=30/1");
  params.m_outCaps = gst_caps_from_string("video/x-raw,format=RGBA,width=1280,height=720,framerate=30/1");

  absl::Status status = priv.PreCapsInit(&params);
  if (!status.ok()) {
    std::cerr << "PreCapsInit failed: " << status << std::endl;
    return 1;
  }

  status = priv.PostCapsInit(&params);
  if (!status.ok()) {
    std::cerr << "PostCapsInit failed: " << status << std::endl;
    return 2;
  }

  if (params.m_inCaps) {
    gst_caps_unref(params.m_inCaps);
    params.m_inCaps = nullptr;
  }
  if (params.m_outCaps) {
    gst_caps_unref(params.m_outCaps);
    params.m_outCaps = nullptr;
  }

  cudaStreamDestroy(stream);
  return 0;
}
