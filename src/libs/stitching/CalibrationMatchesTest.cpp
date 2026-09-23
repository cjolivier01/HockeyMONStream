#include "hstream/src/libs/stitching/CalibrationMatches.h"

#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include <opencv2/imgcodecs.hpp>

namespace {
namespace fs = std::filesystem;
using namespace hm::stitching;

void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <typename T>
T take(absl::StatusOr<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().ToString());
  return std::move(*result);
}
void write(const fs::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::binary);
  output << contents;
  require(output.good(), "Cannot write fixture");
}
std::string read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "Cannot read fixture");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
struct TemporaryDirectory {
  fs::path path;
  TemporaryDirectory() {
    char pattern[] = "/tmp/hstream-calibration-matches-XXXXXX";
    const char* created = ::mkdtemp(pattern);
    require(created, "Cannot create fixture directory");
    path = created;
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
YAML::Node config() {
  return YAML::Load(
      "stitching:\n  stitch_frame_time: '00:00:03'\n  control_point_matcher: superpoint-lightglue\n"
      "game:\n  videos:\n    left: [cam1.mp4]\n    right: [cam2.mp4]\n"
      "  stitching:\n    frame_offsets: {left: 0, right: 0.25}\n");
}
CalibrationMatchSet fixture(const fs::path& game) {
  CalibrationMatchSet set;
  set.source_context = take(CalibrationMatchSourceContext(config(), game));
  for (size_t pair = 0; pair < 2; ++pair) {
    CalibrationMatchFrame frame;
    for (size_t camera = 0; camera < 2; ++camera) {
      frame.images[camera] = game / (std::to_string(pair) + "-" + std::to_string(camera) + ".png");
      frame.sizes[camera] = {200, 160};
      frame.source_paths[camera] = "cam" + std::to_string(camera + 1) + ".mp4";
      frame.source_seconds[camera] = 3 + pair + camera * 0.25;
      require(
          cv::imwrite(frame.images[camera].string(), cv::Mat(160, 200, CV_8UC3, cv::Scalar(10 + pair, 20, 30))),
          "Cannot write input PNG");
    }
    frame.matches = {{{25.25f, 30.5f}, {27.75f, 31.25f}, 0.9f}, {{65.5f, 90.25f}, {70.5f, 95.25f}, 0.7f}};
    set.frames.push_back(std::move(frame));
  }
  return set;
}
void roundtrip_and_reset(const fs::path& game, const fs::path& copied) {
  auto automatic = fixture(game);
  const auto fingerprint = take(PublishCalibrationMatches(game, automatic));
  require(
      take(PublishCalibrationMatches(game, automatic)) == fingerprint, "Publishing identical content must be stable");
  const auto original = take(LoadCalibrationMatches(game, fingerprint));
  require(
      original.frames.size() == 2 && original.input_fingerprint.size() == 64 && !original.manual &&
          original.automatic_fingerprint.empty() && original.frames[1].source_seconds[1] == 4.25 &&
          original.frames[0].matches[0].left == automatic.frames[0].matches[0].left,
      "Automatic matches and input identity must roundtrip");
  auto edited = original;
  edited.manual = true;
  edited.automatic_fingerprint = fingerprint;
  edited.frames[0].matches.erase(edited.frames[0].matches.begin());
  edited.frames[1].matches.push_back({{100.25f, 110.5f}, {99.25f, 111.5f}, 1.0f});
  const auto manual_fingerprint = take(PublishCalibrationMatches(game, edited));
  const auto manual = take(LoadCalibrationMatches(game, manual_fingerprint));
  require(
      manual_fingerprint != fingerprint && manual.manual && manual.input_fingerprint == original.input_fingerprint &&
          manual.frames[0].matches.size() == 1 && manual.frames[1].matches.size() == 3,
      "Manual edits must preserve shared input ownership");
  const auto reset = take(LoadCalibrationMatches(game, manual.automatic_fingerprint));
  require(
      reset.fingerprint == fingerprint && reset.frames[0].matches.size() == 2 && reset.frames[1].matches.size() == 2,
      "Reset must load the untouched automatic matches");
  require(CopyCalibrationMatches(game, copied, manual_fingerprint).ok(), "Copy manual and automatic dependencies");
  const auto copy = take(LoadCalibrationMatches(copied, manual_fingerprint));
  require(
      copy.automatic_fingerprint == fingerprint && fs::exists(copy.frames[0].images[0]) &&
          take(LoadCalibrationMatches(copied, fingerprint)).frames[0].matches.size() == 2,
      "Copied manual set must retain a usable automatic reset");

  auto invalid = edited;
  invalid.automatic_fingerprint = manual_fingerprint;
  require(!PublishCalibrationMatches(game, invalid).ok(), "An edited set cannot serve as an automatic original");
  invalid.automatic_fingerprint = std::string(64, 'a');
  require(!PublishCalibrationMatches(game, invalid).ok(), "A missing automatic original must fail publication");
  invalid = edited;
  invalid.matcher = ControlPointMatcher::kLoFTR;
  require(!PublishCalibrationMatches(game, invalid).ok(), "Manual set cannot change its original matcher");
  invalid = edited;
  invalid.source_context = std::string(64, 'b');
  require(!PublishCalibrationMatches(game, invalid).ok(), "Manual set cannot change its original sources");
  invalid = edited;
  invalid.frames[0].source_seconds[0] += 1;
  invalid.input_fingerprint.clear();
  require(!PublishCalibrationMatches(game, invalid).ok(), "Manual set cannot substitute original input identity");
  invalid = original;
  invalid.automatic_fingerprint = fingerprint;
  require(!PublishCalibrationMatches(game, invalid).ok(), "Automatic sets cannot carry reset-reference chains");

  // A leftover predictable pending-name symlink must never redirect publication.
  const auto sets = copied / "calibration-matches" / "sets";
  fs::remove(sets / (manual_fingerprint + ".yaml"));
  const auto unrelated = copied / "unrelated.txt";
  write(unrelated, "preserve me");
  fs::create_symlink(unrelated, sets / (".pending-" + manual_fingerprint));
  require(
      take(PublishCalibrationMatches(copied, manual)) == manual_fingerprint && read(unrelated) == "preserve me",
      "Publication must not follow a predictable pending-file symlink");

  const auto original_path = game / "calibration-matches" / "sets" / (fingerprint + ".yaml");
  const auto bytes = read(original_path);
  write(original_path, bytes + "# changed\n");
  require(
      !LoadCalibrationMatches(game, manual_fingerprint, false).ok(),
      "Manual load must validate its automatic document even without rehashing images");
  write(original_path, bytes);
  const auto image = original.frames[0].images[0];
  const auto image_bytes = read(image);
  write(image, image_bytes + "changed");
  require(!LoadCalibrationMatches(game, fingerprint).ok(), "Stored image tampering must fail verified load");
  require(!PublishCalibrationMatches(game, edited).ok(), "Edits cannot publish after inspected inputs change");
  write(image, image_bytes);
  const auto manifest = image.parent_path() / "frames.yaml";
  const auto manifest_bytes = read(manifest);
  write(manifest, manifest_bytes + "# changed\n");
  require(!LoadCalibrationMatches(game, fingerprint, false).ok(), "Input manifest integrity is always checked");
  write(manifest, manifest_bytes);
  require(!LoadCalibrationMatches(game, "../escape").ok(), "Fingerprint paths cannot traverse the store");
  fs::remove(image);
  fs::create_symlink(automatic.frames[0].images[0], image);
  require(!LoadCalibrationMatches(game, fingerprint).ok(), "Stored PNG symlinks must be rejected");
}
void metadata_and_bounds(const fs::path& game) {
  auto set = fixture(game);
  const auto rejected = [&](const CalibrationMatchSet& candidate) {
    require(!PublishCalibrationMatches(game, candidate).ok(), "Invalid match metadata must be rejected");
  };
  auto bad = set;
  bad.frames[0].matches[0].left.x = 200;
  rejected(bad);
  bad = set;
  bad.frames[0].matches[0].right.y = -1;
  rejected(bad);
  bad = set;
  bad.frames[0].matches[0].score = std::numeric_limits<float>::quiet_NaN();
  rejected(bad);
  bad = set;
  bad.frames[0].matches.resize(kMaximumEditableMatchesPerPair + 1);
  rejected(bad);
  bad = set;
  bad.frames.resize(17);
  rejected(bad);
  bad = set;
  bad.frames[0].source_seconds[0] = -1;
  rejected(bad);
  bad = set;
  bad.frames[0].sizes[0].width = 32769;
  rejected(bad);
  bad = set;
  bad.matcher = static_cast<ControlPointMatcher>(-1);
  rejected(bad);
  bad = set;
  bad.input_fingerprint = "invalid";
  rejected(bad);

  const auto minimal = YAML::Load("game: {videos: {left: [cam1.mp4], right: [cam2.mp4]}}");
  auto defaulted_set = set;
  defaulted_set.source_context = take(CalibrationMatchSourceContext(minimal, game));
  require(
      ValidateCalibrationMatchInputs(defaulted_set, minimal, game, 2).ok(),
      "Absent optional stitching, anchor, matcher and offsets must use defaults");
  auto equivalent = YAML::Clone(minimal);
  equivalent["stitching"]["stitch_frame_time"] = "00:00:00";
  equivalent["game"]["stitching"]["frame_offsets"]["left"] = 0.0;
  equivalent["game"]["stitching"]["frame_offsets"]["right"] = 0.0;
  require(
      take(CalibrationMatchSourceContext(equivalent, game)) == defaulted_set.source_context,
      "Missing synchronization and anchor must match explicit zero defaults");
  equivalent = YAML::Clone(minimal);
  equivalent["game"]["stitching"] = YAML::Load("{}");
  require(
      take(CalibrationMatchSourceContext(equivalent, game)) == defaulted_set.source_context,
      "Missing frame_offsets within an existing game.stitching map must default");
  require(!CalibrationMatchSourceContext(YAML::Load("{}"), game).ok(), "Camera sources remain required");

  auto settings = config();
  require(ValidateCalibrationMatchInputs(set, settings, game, 2).ok(), "Matching source context must validate");
  require(!ValidateCalibrationMatchInputs(set, settings, game, 1).ok(), "Changed frame count must fail");
  for (int change = 0; change < 5; ++change) {
    settings = config();
    if (change == 0)
      settings["stitching"]["stitch_frame_time"] = "00:00:04";
    if (change == 1)
      settings["game"]["stitching"]["frame_offsets"]["right"] = 0.5;
    if (change == 2)
      settings["game"]["videos"]["left"][0] = "other.mp4";
    if (change == 3)
      settings["stitching"]["control_point_matcher"] = "loftr";
    if (change == 4)
      settings["stitching"]["control_point_matcher"] = YAML::Load("[bad, type]");
    require(
        !ValidateCalibrationMatchInputs(set, settings, game, 2).ok(), "Changed capture or matcher context must fail");
  }
  settings = config();
  settings["game"]["videos"]["left"][0] = "";
  require(!CalibrationMatchSourceContext(settings, game).ok(), "Empty camera sources must be rejected");
  settings = config();
  settings["stitching"]["stitch_frame_time"] = "bad";
  require(!CalibrationMatchSourceContext(settings, game).ok(), "Invalid capture time must be rejected");
  settings = config();
  settings["stitching"]["stitch_frame_time"] = YAML::Load("[bad, type]");
  require(
      !CalibrationMatchSourceContext(settings, game).ok(), "Malformed capture time cannot silently use the default");
  settings = config();
  settings["game"]["stitching"]["frame_offsets"]["right"] = "bad";
  require(!CalibrationMatchSourceContext(settings, game).ok(), "Malformed synchronization cannot silently use zero");

  const auto outside = game / "outside";
  fs::create_directory(outside);
  const auto symlink_game = game / "symlink-game";
  fs::create_directory(symlink_game);
  fs::create_directory_symlink(outside, symlink_game / "calibration-matches");
  require(
      !PublishCalibrationMatches(symlink_game, set).ok() && fs::is_empty(outside),
      "Publishing cannot follow a symlinked store root");
}
void coordinate_roundtrip(const fs::path& game) {
  const FisheyeLensCalibration lens{{640, 480}, 450, 460, 320, 240, {0.01, -0.002, 0.0001, 0.0}};
  AkazeMatchingCalibration calibration{lens, lens, std::string(64, 'c')};
  const std::array<cv::Size, 2> sizes{{{640, 480}, {1280, 960}}};
  std::vector<FeatureMatch> matches;
  for (size_t i = 0; i < kMaximumEditableMatchesPerPair; ++i) {
    const cv::Point2f p(200 + i % 150, 180 + i % 90);
    matches.push_back({p, p * 2, 0.9f, static_cast<int>(i), static_cast<int>(i + 1)});
  }
  const auto raw = take(ConvertCalibrationMatchCoordinates(matches, sizes, calibration, true));
  require(cv::norm(raw[0].left - matches[0].left) > 1, "Lens conversion must actually transform coordinates");
  const auto restored = take(ConvertCalibrationMatchCoordinates(raw, sizes, calibration, false));
  for (size_t i = 0; i < matches.size(); ++i) {
    require(
        cv::norm(restored[i].left - matches[i].left) < 0.001 &&
            cv::norm(restored[i].right - matches[i].right) < 0.001 && restored[i].left_index == matches[i].left_index &&
            restored[i].right_index == matches[i].right_index && restored[i].score == matches[i].score,
        "Batched lens roundtrip must preserve points, ordering, scores and ids at unequal camera sizes");
  }
  require(
      take(ConvertCalibrationMatchCoordinates(matches, sizes, {}, false))[0].left == matches[0].left,
      "Uncalibrated coordinates must remain unchanged");
  auto invalid = matches;
  invalid[0].left.x = -1;
  require(
      !ConvertCalibrationMatchCoordinates(invalid, sizes, {}, false).ok(), "Uncalibrated endpoints still need bounds");
  require(
      !ConvertCalibrationMatchCoordinates({}, {{{0, 10}, {10, 10}}}, {}, false).ok(), "Empty batches need valid sizes");
  auto one_lens = calibration;
  one_lens.right.reset();
  require(!ConvertCalibrationMatchCoordinates(matches, sizes, one_lens, true).ok(), "One-sided calibration must fail");
  auto invalid_lens = calibration;
  invalid_lens.left->fx = 0;
  require(
      !ConvertCalibrationMatchCoordinates(matches, sizes, invalid_lens, false).ok(),
      "Invalid lens focal length must fail");

  auto set = fixture(game);
  set.matcher = ControlPointMatcher::kAkazeHamming;
  set.calibration = calibration;
  const auto fingerprint = take(PublishCalibrationMatches(game, set));
  const auto loaded = take(LoadCalibrationMatches(game, fingerprint));
  require(ValidateCalibrationMatchCalibration(loaded, calibration).ok(), "Lens metadata must survive persistence");
  invalid_lens = calibration;
  invalid_lens.right->cx += 1;
  require(
      !ValidateCalibrationMatchCalibration(loaded, invalid_lens).ok(),
      "Lens values must match even with the same profile hash");
  invalid_lens = calibration;
  invalid_lens.source_profile_fingerprint = std::string(64, 'd');
  require(!ValidateCalibrationMatchCalibration(loaded, invalid_lens).ok(), "Changed lens profile identity must fail");
  set.matcher = ControlPointMatcher::kLoFTR;
  require(!PublishCalibrationMatches(game, set).ok(), "Neural matches cannot be tagged with rectifying lenses");
}
} // namespace

int main() {
  try {
    TemporaryDirectory temporary;
    for (const auto* name : {"roundtrip", "copied", "bounds", "coordinates"})
      fs::create_directory(temporary.path / name);
    roundtrip_and_reset(temporary.path / "roundtrip", temporary.path / "copied");
    metadata_and_bounds(temporary.path / "bounds");
    coordinate_roundtrip(temporary.path / "coordinates");
    std::cout << "Calibration match persistence and coordinate checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
