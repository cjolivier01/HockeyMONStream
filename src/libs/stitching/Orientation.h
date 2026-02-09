#pragma once

#include <map>
#include <string>

#include "absl/status/statusor.h"

namespace hm {
namespace stitching {

/// Mapping from chapter number to filename.
using VideoChapter = std::map<int, std::string>;
/// Videos dictionary mapping a video key (as string) to its chapters.
using VideosDict = std::map<std::string, VideoChapter>;

absl::StatusOr<VideosDict> get_available_videos(const std::string& dir_name, bool prune = false);

} // namespace stitching
} // namespace hm
