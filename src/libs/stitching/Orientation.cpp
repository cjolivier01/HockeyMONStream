
/**
 * @brief Provides functions for parsing video filenames and building a video dependency dictionary.
 *
 * This module implements functions equivalent to the Python version. It uses std::regex
 * and std::filesystem to find matching files in a directory and then extracts video/chapter numbers
 * from filenames following the GoPro naming pattern as well as left/right file patterns.
 */

#include "hstream/src/libs/stitching/Orientation.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <regex>
#include <string>

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

/** @brief Regular expression for left part files.
 *
 * Pattern: left-[0-9]\.mp4$
 */
constexpr const char* LEFT_PART_FILE_PATTERN = R"(left-[0-9]\.mp4$)";

/** @brief Regular expression for right part files.
 *
 * Pattern: right-[0-9]\.mp4$
 */
constexpr const char* RIGHT_PART_FILE_PATTERN = R"(right-[0-9]\.mp4$)";

/** @brief Regular expression for a plain left file.
 *
 * Pattern: left.mp4
 */
constexpr const char* LEFT_FILE_PATTERN = R"(left\.mp4$)";

/** @brief Regular expression for a plain right file.
 *
 * Pattern: right.mp4
 */
constexpr const char* RIGHT_FILE_PATTERN = R"(right\.mp4$)";

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
  size_t lastPos = 0;
  while ((pos = name.find('-', lastPos)) != std::string::npos) {
    lastPos = pos + 1;
  }
  // The last token is after the final '-'
  return std::stoi(name.substr(lastPos));
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
std::vector<std::string> find_matching_files(const std::string& re_pattern, const std::string& directory) {
  std::regex pattern(re_pattern);
  std::vector<std::string> matching_files;

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
VideosDict get_available_videos(const std::string& dir_name, bool prune) {
  VideosDict videos_dict;

  // Find GoPro files.
  std::vector<std::string> gopro_files = find_matching_files(GOPRO_FILE_PATTERN, dir_name);
  // For each GoPro file, extract video and chapter numbers.
  for (const auto& file : gopro_files) {
    auto [video, chapter] = gopro_get_video_and_chapter(file);
    // Use the video number as a string key.
    std::string video_key = std::to_string(video);
    videos_dict[video_key][chapter] = file;
  }

  // Process left files.
  std::vector<std::string> files = find_matching_files(LEFT_FILE_PATTERN, dir_name);
  if (!files.empty()) {
    // Expect exactly one plain left file.
    assert(files.size() == 1);
    videos_dict["left"][1] = files[0];
  } else {
    files = find_matching_files(LEFT_PART_FILE_PATTERN, dir_name);
    if (!files.empty()) {
      for (const auto& file : files) {
        int part_num = get_lr_part_number(file);
        videos_dict["left"][part_num] = file;
      }
    }
  }

  // Process right files.
  files = find_matching_files(RIGHT_FILE_PATTERN, dir_name);
  if (!files.empty()) {
    // Expect exactly one plain right file.
    assert(files.size() == 1);
    videos_dict["right"][1] = files[0];
  } else {
    files = find_matching_files(RIGHT_PART_FILE_PATTERN, dir_name);
    if (!files.empty()) {
      for (const auto& file : files) {
        int part_num = get_lr_part_number(file);
        videos_dict["right"][part_num] = file;
      }
    }
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
