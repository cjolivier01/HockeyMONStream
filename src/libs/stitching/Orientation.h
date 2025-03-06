#pragma once

#include <map>
#include <string>

namespace hm {
namespace stitching {

/// Mapping from chapter number to filename.
using VideoChapter = std::map<int, std::string>;
/// Videos dictionary mapping a video key (as string) to its chapters.
using VideosDict = std::map<std::string, VideoChapter>;

VideosDict get_available_videos(const std::string& dir_name, bool prune = false);

} // namespace stitching
} // namespace hm
