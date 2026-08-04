
/**
 * @brief Provides functions for parsing video filenames and building a video dependency dictionary.
 *
 * This module implements functions equivalent to the Python version. It uses std::regex
 * and std::filesystem to find matching files in a directory and then extracts video/chapter numbers
 * from filenames following the GoPro naming pattern as well as left/right file patterns.
 */

#include "hstream/src/libs/stitching/Orientation.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

#include <opencv2/videoio.hpp>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

namespace hm {
namespace stitching {

// For convenience.
namespace fs = std::filesystem;

namespace {
/** @brief Regular expression for GoPro files.
 *
 * Pattern: ^G[A-Z][0-9]{6}\.(MP4|mp4)$
 */
constexpr const char* GOPRO_FILE_PATTERN = R"(^G[A-Z][0-9]{6}\.(MP4|mp4)$)";

/** @brief Regular expression for Insta360 files.
 *
 * Pattern: ^VID_[0-9]{8}_[0-9]{6}_[0-9]{3}\.(MP4|mp4)$
 */
constexpr const char* INSTA360_FILE_PATTERN = R"(^(VID_[0-9]{8}_[0-9]{6}_[0-9]{3}\.(MP4|mp4))$)";

/** @brief Regular expression for left part files.
 *
 * Pattern: left-[0-9]\.mp4$
 */
constexpr const char* LEFT_PART_FILE_PATTERN = R"(left-[0-9]+\.(mp4|mkv|m4v)$)";

/** @brief Regular expression for right part files.
 *
 * Pattern: right-[0-9]\.mp4$
 */
constexpr const char* RIGHT_PART_FILE_PATTERN = R"(right-[0-9]+\.(mp4|mkv|m4v)$)";

/** @brief Regular expression for a plain left file.
 *
 * Pattern: left.mp4
 */
constexpr const char* LEFT_FILE_PATTERN = R"(left\.(mp4|mkv|m4v)$)";

/** @brief Regular expression for a plain right file.
 *
 * Pattern: right.mp4
 */
constexpr const char* RIGHT_FILE_PATTERN = R"(right\.(mp4|mkv|m4v)$)";

/** @brief Regular expression for a pre-stitched file.
 *
 * Pattern: stitched_output-with-audio\.(mp4|mkv)
 */
constexpr const char* STITCHED_FILE_PATTERN = R"(stitched_output-with-audio\.(mp4|mkv)$)";

/**
 * @brief Extracts the video and chapter numbers from a GoPro file name.
 *
 * The GoPro file pattern is assumed to be of the form "GXzzxxxx.mp4" where:
 * - 'G' is the first character.
 * - The second character is either 'H' or 'X'.
 * - Characters at positions 2-3 (zero-indexed) represent the chapter number.
 * - Characters at positions 4-7 represent the video number.
 *
 * @param filename The path of the file.
 * @return A pair (video_number, chapter_number).
 */
std::pair<int, int> gopro_get_video_and_chapter(const fs::path& filename) {
  // Get the stem (filename without extension)
  std::string name = filename.stem().string();
  // Validate the pattern assumptions.
  assert(!name.empty() && name[0] == 'G');
  assert(name.size() >= 8 && (name[1] == 'H' || name[1] == 'X'));
  // Extract video number from characters at positions 4 to 7.
  int video = std::stoi(name.substr(4, 4));
  // Extract chapter number from characters at positions 2 to 3.
  int chapter = std::stoi(name.substr(2, 2));
  return {video, chapter};
}

/**
 * @brief Extracts the video identifier and chapter number from an Insta360 file name.
 *
 * Expected pattern (stem): VID_<YYYYMMDD>_<HHMMSS>_<chapter>
 *
 * @param filename The path of the file.
 * @param out_video_id Output parameter for the combined video identifier.
 * @param out_chapter Output parameter for the chapter number.
 * @return true if parsing succeeded, false otherwise.
 */
bool insta360_get_video_and_chapter(const fs::path& filename, long& out_video_id, int& out_chapter) {
  std::string name = filename.stem().string();
  std::vector<std::string> tokens;
  size_t start = 0;
  while (true) {
    size_t pos = name.find('_', start);
    if (pos == std::string::npos) {
      tokens.emplace_back(name.substr(start));
      break;
    }
    tokens.emplace_back(name.substr(start, pos - start));
    start = pos + 1;
  }
  if (tokens.size() < 4) {
    return false;
  }
  if (tokens[0] != "VID") {
    return false;
  }
  const std::string& date_token = tokens[1];
  const std::string& video_token = tokens[2];
  const std::string& chapter_token = tokens[3];
  try {
    out_video_id = std::stol(date_token + video_token);
    out_chapter = std::stoi(chapter_token);
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

/**
 * @brief Gets the left/right part number from a file name.
 *
 * The function splits the file stem on '-' and returns the last token as an integer.
 *
 * @param filename The filename as a string.
 * @return The part number.
 */
int get_lr_part_number(const std::string& filename) {
  fs::path p(filename);
  std::string name = p.stem().string();
  // Split the name by '-' using find and substr.
  size_t pos = 0;
  size_t last_pos = 0;
  while ((pos = name.find('-', last_pos)) != std::string::npos) {
    last_pos = pos + 1;
  }
  // The last token is after the final '-'
  return std::stoi(name.substr(last_pos));
}

/**
 * @brief Finds all files in a directory that match a given regex pattern.
 *
 * This function iterates over the files in the specified directory and returns
 * a sorted vector of full file paths that match the provided regex.
 *
 * @param re_pattern The regex pattern as a string.
 * @param directory The directory to search.
 * @return A sorted vector of matching file paths.
 */
absl::StatusOr<std::vector<std::string>> find_matching_files(
    const std::string& re_pattern,
    const std::string& directory) {
  std::regex pattern(re_pattern);
  std::vector<std::string> matching_files;
  if (!fs::is_directory(directory)) {
    return absl::InvalidArgumentError(TO_STRING("Directory \"" << directory << " doesn't exist or is not a directory"));
  }
  // Iterate over each file in the directory.
  for (const auto& entry : fs::directory_iterator(directory)) {
    std::string filename = entry.path().filename().string();
    if (std::regex_search(filename, pattern)) {
      matching_files.push_back(entry.path().string());
    }
  }
  std::sort(matching_files.begin(), matching_files.end());
  return matching_files;
}

/**
 * @brief Prunes chapters from the videos dictionary.
 *
 * This function is intended to remove videos that do not have matching chapters.
 * Currently it is a placeholder that returns the original videos dictionary and an empty dictionary.
 *
 * @param videos The videos dictionary.
 * @return A pair where the first element is the pruned videos and the second element is the discarded videos.
 */
std::pair<VideosDict, VideosDict> prune_chapters(const VideosDict& videos) {
  // Placeholder: no pruning is performed.
  return {videos, VideosDict{}};
}

/**
 * @brief Find vendor-specific (GoPro / Insta360) chapter files in a directory.
 *
 * Returns a list of ((video_id, chapter), full_path) sorted by (video_id, chapter).
 */
absl::StatusOr<std::vector<std::pair<std::pair<long, int>, std::string>>> find_vendor_chapter_pairs(
    const std::string& directory) {
  std::vector<std::pair<std::pair<long, int>, std::string>> pairs;

  // GoPro
  std::vector<std::string> files;
  HM_ASSIGN_OR_RETURN(files, find_matching_files(GOPRO_FILE_PATTERN, directory));
  for (const auto& f : files) {
    try {
      auto vc = gopro_get_video_and_chapter(fs::path(f));
      pairs.emplace_back(vc, f);
    } catch (const std::exception&) {
      continue;
    }
  }

  // Insta360
  HM_ASSIGN_OR_RETURN(files, find_matching_files(INSTA360_FILE_PATTERN, directory));
  for (const auto& f : files) {
    long video_id = 0;
    int chapter = 0;
    if (!insta360_get_video_and_chapter(fs::path(f), video_id, chapter)) {
      continue;
    }
    pairs.emplace_back(std::make_pair(video_id, chapter), f);
  }

  std::sort(pairs.begin(), pairs.end());
  return pairs;
}

/**
 * @brief Flatten vendor chapter pairs into a 1..N -> file map.
 */
VideoChapter pairs_to_linear_chapter_map(const std::vector<std::pair<std::pair<long, int>, std::string>>& pairs) {
  VideoChapter chapter_map;
  int chapter_index = 1;
  for (const auto& item : pairs) {
    chapter_map[chapter_index++] = item.second;
  }
  return chapter_map;
}

/**
 * @brief Collect left/right single or part files into a chapter map.
 *
 * If renumber is true, chapters are returned as 1..N in file order; otherwise
 * they retain the part number from the filename (e.g., left-3.mp4 -> 3).
 */
absl::StatusOr<VideoChapter> collect_lr_chapters(const std::string& directory, bool left_side, bool renumber) {
  VideoChapter chapter_map;
  const char* single_pattern = left_side ? LEFT_FILE_PATTERN : RIGHT_FILE_PATTERN;
  const char* parts_pattern = left_side ? LEFT_PART_FILE_PATTERN : RIGHT_PART_FILE_PATTERN;

  // Single file (left.mp4 / right.mp4)
  std::vector<std::string> files;
  HM_ASSIGN_OR_RETURN(files, find_matching_files(single_pattern, directory));
  if (!files.empty()) {
    // Expect exactly one plain file; use the first.
    chapter_map[1] = files.front();
    return chapter_map;
  }

  // Parts (left-1.mp4, left-2.mp4, ...)
  HM_ASSIGN_OR_RETURN(files, find_matching_files(parts_pattern, directory));
  if (!files.empty()) {
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
      return get_lr_part_number(a) < get_lr_part_number(b);
    });

    if (renumber) {
      int chapter_index = 1;
      for (const auto& f : files) {
        chapter_map[chapter_index++] = f;
      }
    } else {
      for (const auto& f : files) {
        chapter_map[get_lr_part_number(f)] = f;
      }
    }
  }

  return chapter_map;
}

/**
 * @brief Collect and order chapter files inside a directory.
 *
 * Strategy: prefer vendor-specific patterns (GoPro, Insta360) if found;
 * otherwise fall back to left/right patterns. Output keys are 1..N in
 * the discovered order.
 */
absl::StatusOr<VideoChapter> collect_chapters_for_dir(const std::string& directory) {
  // Vendor-specific (GoPro, Insta360)
  std::vector<std::pair<std::pair<long, int>, std::string>> pairs;
  HM_ASSIGN_OR_RETURN(pairs, find_vendor_chapter_pairs(directory));
  if (!pairs.empty()) {
    return pairs_to_linear_chapter_map(pairs);
  }

  // Plain left/right within this directory, renumbered 1..N
  VideoChapter chapter_map;
  HM_ASSIGN_OR_RETURN(chapter_map, collect_lr_chapters(directory, /*left_side=*/true, /*renumber=*/true));
  if (!chapter_map.empty()) {
    return chapter_map;
  }
  HM_ASSIGN_OR_RETURN(chapter_map, collect_lr_chapters(directory, /*left_side=*/false, /*renumber=*/true));
  return chapter_map;
}

int cam_index(const std::string& name) {
  for (size_t i = 0; i < name.size(); ++i) {
    if (std::isdigit(static_cast<unsigned char>(name[i]))) {
      size_t j = i;
      while (j < name.size() && std::isdigit(static_cast<unsigned char>(name[j]))) {
        ++j;
      }
      try {
        return std::stoi(name.substr(i, j - i));
      } catch (const std::exception&) {
        return 0;
      }
    }
  }
  return 0;
}

absl::Status save_orientation_config(const fs::path& game_dir, const VideoChapter& left, const VideoChapter& right) {
  if (left.empty() || right.empty()) {
    return absl::InvalidArgumentError("Both camera orientations need at least one chapter");
  }
  if (left.size() != right.size()) {
    return absl::InvalidArgumentError("Left and right cameras have different chapter counts");
  }
  std::vector<std::string> left_paths;
  std::vector<std::string> right_paths;
  for (const auto& [chapter, left_path] : left) {
    const auto found = right.find(chapter);
    if (found == right.end()) {
      return absl::InvalidArgumentError("Left and right cameras have mismatched chapter numbers");
    }
    std::error_code error;
    fs::path relative_left = fs::relative(left_path, game_dir, error);
    if (error)
      return absl::InternalError("Failed to relativize left video path: " + error.message());
    fs::path relative_right = fs::relative(found->second, game_dir, error);
    if (error)
      return absl::InternalError("Failed to relativize right video path: " + error.message());
    left_paths.push_back(relative_left.string());
    right_paths.push_back(relative_right.string());
  }

  const fs::path config_path = game_dir / "config.yaml";
  YAML::Node config(YAML::NodeType::Map);
  try {
    if (fs::is_regular_file(config_path))
      config = YAML::LoadFile(config_path.string());
    config["game"]["videos"]["left"] = left_paths;
    config["game"]["videos"]["right"] = right_paths;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Failed to update private game config: " + std::string(error.what()));
  }

  const fs::path temporary = config_path.string() + ".tmp." + std::to_string(::getpid());
  {
    std::ofstream output(temporary, std::ios::out | std::ios::trunc);
    if (!output.is_open())
      return absl::InternalError("Failed to create temporary game config: " + temporary.string());
    output << config << '\n';
    output.flush();
    if (!output.good()) {
      output.close();
      std::error_code ignored;
      fs::remove(temporary, ignored);
      return absl::InternalError("Failed to write temporary game config: " + temporary.string());
    }
  }
  std::error_code error;
  fs::rename(temporary, config_path, error);
  if (error) {
    fs::remove(temporary, error);
    return absl::InternalError("Failed to atomically publish game config: " + error.message());
  }
  return absl::OkStatus();
}
} // namespace

/**
 * @brief Retrieves available videos in the given directory.
 *
 * The function searches for files that match the GoPro pattern as well as left/right file patterns.
 * It builds a dictionary mapping each video (or left/right identifier) to a mapping of chapter numbers to filenames.
 *
 * @param dir_name The directory name.
 * @param prune If true, prunes videos that don't have matching chapters.
 * @return The videos dictionary.
 */
absl::StatusOr<VideosDict> get_available_videos(const std::string& dir_name, bool prune) {
  if (!fs::is_directory(dir_name)) {
    return absl::InvalidArgumentError(
        TO_STRING("Directory \"" << dir_name << "\" doesn't exist or is not a directory"));
  }

  VideosDict videos_dict;

  // Prefer camera-specific subdirectories cam1, cam2, ... when present.
  std::vector<std::string> cam_dirs;
  std::regex cam_pattern(R"(cam[0-9]+)", std::regex::icase);
  for (const auto& entry : fs::directory_iterator(dir_name)) {
    if (entry.is_symlink() || !entry.is_directory()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (std::regex_match(name, cam_pattern)) {
      cam_dirs.push_back(name);
    }
  }

  if (!cam_dirs.empty()) {
    std::sort(cam_dirs.begin(), cam_dirs.end(), [](const std::string& a, const std::string& b) {
      return cam_index(a) < cam_index(b);
    });

    for (const auto& cam_name : cam_dirs) {
      std::string cam_path = (fs::path(dir_name) / cam_name).string();
      VideoChapter chapter_map;
      HM_ASSIGN_OR_RETURN(chapter_map, collect_chapters_for_dir(cam_path));
      if (!chapter_map.empty()) {
        videos_dict[cam_name] = std::move(chapter_map);
      }
    }

    if (!videos_dict.empty()) {
      if (prune) {
        auto [pruned, discarded] = prune_chapters(videos_dict);
        if (!discarded.empty()) {
          std::cout << "Discarding videos:" << std::endl;
          for (const auto& [key, _] : discarded) {
            std::cout << "Key: " << key << std::endl;
          }
        }
        return pruned;
      }
      return videos_dict;
    }
  }

  // Fallback: scan the root directory.
  std::vector<std::pair<std::pair<long, int>, std::string>> vendor_pairs;
  HM_ASSIGN_OR_RETURN(vendor_pairs, find_vendor_chapter_pairs(dir_name));
  for (const auto& item : vendor_pairs) {
    long video_id = item.first.first;
    int chapter = item.first.second;
    const std::string& file = item.second;
    std::string video_key = std::to_string(video_id);
    videos_dict[video_key][chapter] = file;
  }

  // Plain left/right in the root directory (non-renumbered chapters).
  VideoChapter left_map;
  HM_ASSIGN_OR_RETURN(left_map, collect_lr_chapters(dir_name, /*left_side=*/true, /*renumber=*/false));
  if (!left_map.empty()) {
    videos_dict["left"] = left_map;
  }
  VideoChapter right_map;
  HM_ASSIGN_OR_RETURN(right_map, collect_lr_chapters(dir_name, /*left_side=*/false, /*renumber=*/false));
  if (!right_map.empty()) {
    videos_dict["right"] = right_map;
  }

  // Process any pre-stitched files.
  std::vector<std::string> stitched_files;
  HM_ASSIGN_OR_RETURN(stitched_files, find_matching_files(STITCHED_FILE_PATTERN, dir_name));
  if (!stitched_files.empty()) {
    std::sort(stitched_files.begin(), stitched_files.end());
    videos_dict["stitched"][1] = stitched_files.back();
  }

  // Optionally prune chapters.
  if (prune) {
    auto [pruned, discarded] = prune_chapters(videos_dict);
    if (!discarded.empty()) {
      std::cout << "Discarding videos:" << std::endl;
      for (const auto& [key, chapters] : discarded) {
        std::cout << "Key: " << key << std::endl;
      }
    }
    return pruned;
  }

  return videos_dict;
}

absl::StatusOr<OrientationScores> rink_orientation_scores(const cv::Mat& binary_mask) {
  if (binary_mask.empty() || binary_mask.channels() != 1) {
    return absl::InvalidArgumentError("Orientation requires a non-empty single-channel rink mask");
  }
  cv::Mat mask;
  cv::compare(binary_mask, 0, mask, cv::CMP_GT);
  const int band_width = mask.cols / 8;
  const int band_height = mask.rows / 8;
  if (band_width <= 0 || band_height <= 0) {
    return absl::InvalidArgumentError("Orientation rink mask is too small");
  }
  return OrientationScores{
      static_cast<double>(cv::countNonZero(mask(cv::Rect(0, 0, band_width, mask.rows)))),
      static_cast<double>(cv::countNonZero(mask(cv::Rect(mask.cols - band_width, 0, band_width, mask.rows)))),
      static_cast<double>(cv::countNonZero(mask(cv::Rect(0, 0, mask.cols, band_height)))),
      static_cast<double>(cv::countNonZero(mask(cv::Rect(0, mask.rows - band_height, mask.cols, band_height)))),
  };
}

absl::StatusOr<std::string> classify_rink_orientation(const cv::Mat& binary_mask) {
  auto scores = rink_orientation_scores(binary_mask);
  if (!scores.ok())
    return scores.status();
  if (scores->left > scores->right)
    return std::string("right");
  if (scores->right > scores->left)
    return std::string("left");
  return absl::FailedPreconditionError(
      "Ambiguous camera orientation: left edge sum=" + std::to_string(scores->left) +
      ", right edge sum=" + std::to_string(scores->right));
}

absl::Status configure_game_orientation(const std::string& game_dir_string, const RinkSegmentation& rink_model) {
  const fs::path game_dir(game_dir_string);
  auto videos = get_available_videos(game_dir.string());
  if (!videos.ok())
    return videos.status();
  if (videos->count("left") && videos->count("right")) {
    return save_orientation_config(game_dir, videos->at("left"), videos->at("right"));
  }

  std::map<std::string, VideoChapter> oriented;
  for (const auto& [camera, chapters] : *videos) {
    if (camera == "stitched" || chapters.empty())
      continue;
    // Preserve HockeyMOM's current selection semantics: the minimum pathname,
    // rather than the minimum chapter number, supplies the orientation frame.
    const auto selected = std::min_element(
        chapters.begin(), chapters.end(), [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
    cv::VideoCapture capture(selected->second);
    if (!capture.isOpened()) {
      return absl::NotFoundError("Failed to open orientation video: " + selected->second);
    }
    cv::Mat first_frame;
    if (!capture.read(first_frame) || first_frame.empty()) {
      return absl::InternalError("Failed to decode the first orientation frame: " + selected->second);
    }
    auto rink = rink_model.Infer(first_frame, 0.5);
    if (!rink.ok()) {
      return absl::Status(
          rink.status().code(),
          "Rink inference failed for camera " + camera + ": " + std::string(rink.status().message()));
    }
    auto scores = rink_orientation_scores(rink->combined_mask);
    if (!scores.ok())
      return scores.status();
    auto orientation = classify_rink_orientation(rink->combined_mask);
    if (!orientation.ok()) {
      return absl::Status(
          orientation.status().code(),
          "Camera " + camera + " orientation failed (left=" + std::to_string(scores->left) +
              ", right=" + std::to_string(scores->right) + "): " + std::string(orientation.status().message()));
    }
    std::cout << "Camera " << camera << " orientation=" << *orientation << " left_edge=" << scores->left
              << " right_edge=" << scores->right << std::endl;
    if (oriented.count(*orientation)) {
      return absl::FailedPreconditionError("Multiple cameras classified as " + *orientation);
    }
    oriented[*orientation] = chapters;
  }
  if (!oriented.count("left") || !oriented.count("right") || oriented.size() != 2) {
    return absl::FailedPreconditionError("Native orientation did not identify exactly one left and one right camera");
  }
  return save_orientation_config(game_dir, oriented.at("left"), oriented.at("right"));
}

/**
 * @brief Example main function demonstrating the usage of get_available_videos.
 *
 * This function retrieves available videos from a given directory and prints the results.
 *
 * @return int Exit code.
 */
#if 0
int main() {
  // Example directory (modify as needed)
  std::string directory = "path/to/your/video/directory";

  // Retrieve available videos without pruning.
  VideosDict videos = get_available_videos(directory, false);

  // Print out the videos dictionary.
  for (const auto& [key, chapters] : videos) {
    std::cout << "Video key: " << key << std::endl;
    for (const auto& [chapter, filename] : chapters) {
      std::cout << "  Chapter " << chapter << ": " << filename << std::endl;
    }
  }

  return 0;
}
#endif
} // namespace stitching
} // namespace hm
