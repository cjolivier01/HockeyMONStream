#pragma once

#include "hstream/src/libs/playtracker_replay/ReplaySession.h"

#include <gst/gst.h>
#include <yaml-cpp/yaml.h>
#include <memory>

// Resolve persisted aliases/null defaults with the production configuration
// semantics. Automatic grading requires every original chapter to be >=10 bit.
absl::Status ResolveExperimentStitchingSettings(
    const YAML::Node& config,
    bool automatic_high_bit_depth,
    hm::playtracker_replay::StitchingMedia* media);

// Resolves the previously configured left/right playlists and validates the
// existing maps. Never recalibrates a historical experiment implicitly.
absl::StatusOr<hm::playtracker_replay::StitchingMedia> PrepareExperimentSources(
    const std::string& directory,
    unsigned width,
    unsigned height);

// Production URI playlist pairing and stitching, with no inference, encoding,
// audio output, or CPU video-frame access.
class CameraExperimentSource {
 public:
  CameraExperimentSource();
  ~CameraExperimentSource();
  bool Build(
      GstElement* graph,
      const hm::playtracker_replay::StitchingMedia& media,
      std::uint64_t start_ns,
      std::string* error);
  GstElement* output() const;
  void Suspend();
  void Resume();
  void Cancel();
  bool Position();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
