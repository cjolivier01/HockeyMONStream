#pragma once

#include <gst/gst.h>
#include <yaml-cpp/yaml.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "absl/status/statusor.h"

struct _AppCtx;
struct NvBufSurface;

namespace hm::pipeline {

// Offline-only observer. GPU resize bounds readback to detector resolution;
// ordinary Program pipelines never construct this class.
class Int8FrameSampler {
 public:
  static absl::StatusOr<std::unique_ptr<Int8FrameSampler>> Create(
      _AppCtx* app,
      const std::filesystem::path& game,
      const std::filesystem::path& output,
      uint64_t duration_ns,
      size_t count,
      int width,
      int height);
  ~Int8FrameSampler();
  bool complete() const {
    return complete_.load();
  }
  absl::Status Finish();
  static absl::Status Verify(const std::filesystem::path& directory);

 private:
  static GstPadProbeReturn Observe(GstPad*, GstPadProbeInfo*, gpointer);
  absl::Status Sample(GstBuffer* buffer);
  _AppCtx* app_{nullptr};
  std::filesystem::path game_, output_;
  std::string generation_;
  std::vector<uint64_t> times_;
  size_t next_{0};
  int width_{0}, height_{0};
  GstPad* pad_{nullptr};
  gulong probe_{0};
  std::atomic<bool> complete_{false};
  YAML::Node manifest_;
  absl::Status failure_;
};

} // namespace hm::pipeline
