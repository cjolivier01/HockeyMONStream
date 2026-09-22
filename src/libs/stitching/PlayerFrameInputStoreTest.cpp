#include "hstream/src/libs/stitching/PlayerFrameInputStore.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

namespace fs = std::filesystem;
using namespace hm::stitching;

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

std::string read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("Cannot read fixture: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_bytes(const fs::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), contents.size());
  output.close();
  if (!output)
    throw std::runtime_error("Cannot write fixture: " + path.string());
}

PlayerFrameSelectionPlan make_plan(const fs::path& root) {
  PlayerFrameSelectionPlan plan;
  plan.settings.frame_count = 2;
  plan.context = {
      {"source_context", "fixed-camera-chapters-offsets-anchor"},
      {"baseline_generation", "baseline"},
      {"output_generation", "output"},
      {"detector_identity", "detector"},
      {"rink_mask_sha256", "mask-content"},
      {"rink_mask_revision", "output:authority"},
      {"fieldmask_settings", "center=-0.1;bottom=0.1"},
      {"output_rotation_degrees", "0"}};
  for (const char* role : {"left", "right"}) {
    const fs::path video = root / (std::string(role) + ".mp4");
    write_bytes(video, "recorded-source-fixture");
    auto binding = BindPlayerFrameSource(video);
    if (!binding.ok())
      throw std::runtime_error(binding.status().ToString());
    plan.sources.push_back(*binding);
  }
  for (uint64_t index = 0; index < 2; ++index) {
    PlayerFrameObservation observation;
    observation.pair.timeline_pts_ns = (10 + index * 2) * kPlayerFrameSecond;
    for (size_t camera = 0; camera < 2; ++camera) {
      observation.pair.cameras[camera] = {
          plan.sources[camera].path,
          observation.pair.timeline_pts_ns + camera * 123,
          static_cast<uint32_t>(camera),
          300 + index * 60};
    }
    if (index > 0) {
      observation.eligible_people = 1;
      observation.size_band_counts[0] = 1;
      observation.coverage = {7};
      observation.quality = 0.9;
    }
    plan.selected.push_back(observation);
  }
  auto fingerprint = PlayerFrameSelectionFingerprint(plan);
  if (!fingerprint.ok())
    throw std::runtime_error(fingerprint.status().ToString());
  plan.fingerprint = *fingerprint;
  const auto status = ValidatePlayerFrameSelectionPlan(plan);
  if (!status.ok())
    throw std::runtime_error(status.ToString());
  return plan;
}

std::vector<std::array<fs::path, 2>> make_images(const fs::path& root) {
  fs::create_directories(root);
  std::vector<std::array<fs::path, 2>> images(2);
  for (size_t index = 0; index < images.size(); ++index) {
    for (size_t camera = 0; camera < 2; ++camera) {
      images[index][camera] = root / (std::to_string(index) + "-" + std::to_string(camera) + ".png");
      const cv::Mat image = index == 0 ? cv::Mat(48, 1536, CV_8UC3, cv::Scalar(19 + camera, 70, 151))
                                       : cv::Mat(48, 1536, CV_16UC3, cv::Scalar(4096 + camera * 256, 16384, 32768));
      if (!cv::imwrite(images[index][camera].string(), image, {cv::IMWRITE_PNG_COMPRESSION, 0}))
        throw std::runtime_error("Cannot create PNG fixture");
    }
  }
  return images;
}

fs::path bundle_path(const fs::path& game, const PlayerFrameSelectionPlan& plan) {
  return game / "player-frame-inputs" / plan.fingerprint;
}

void clone_bundle(const fs::path& source, const fs::path& destination) {
  fs::create_directories(destination);
  fs::copy(source / "player-frame-inputs", destination / "player-frame-inputs", fs::copy_options::recursive);
}

bool run(const fs::path& root) {
  bool ok = true;
  const auto plan = make_plan(root);
  const auto images = make_images(root / "captured");
  const fs::path source = root / "source-game";
  fs::create_directories(source);
  auto absent = LoadPlayerFrameInputs(source, plan);
  ok &= expect(absent.ok() && !absent->has_value(), "only an absent bundle returns no input set");
  const fs::path destination = root / "copied-game";
  fs::create_directories(destination);
  ok &= expect(
      absl::IsNotFound(CopyPlayerFrameInputs(source, destination, plan)), "copy reports absent inputs explicitly");

  auto published = PublishPlayerFrameInputs(source, plan, images);
  if (!expect(published.ok(), "publish a complete two-pair bundle: " + published.ToString()))
    return false;
  auto loaded = LoadPlayerFrameInputs(source, plan);
  if (!expect(loaded.ok() && loaded->has_value(), "load a published bundle"))
    return false;
  const auto& inputs = **loaded;
  if (!expect(inputs.images.size() == 2 && inputs.thumbnails.size() == 2, "retain every ordered camera pair"))
    return false;
  for (size_t index = 0; index < images.size(); ++index) {
    for (size_t camera = 0; camera < 2; ++camera) {
      const cv::Mat original = cv::imread(images[index][camera].string(), cv::IMREAD_UNCHANGED);
      const cv::Mat retained = cv::imread(inputs.images[index][camera].string(), cv::IMREAD_UNCHANGED);
      ok &= expect(
          retained.type() == original.type() && retained.size() == original.size() &&
              cv::norm(original, retained, cv::NORM_INF) == 0,
          "full inputs preserve native resolution, bit depth and pixels");
      ok &= expect(
          read_bytes(inputs.images[index][camera]) == read_bytes(images[index][camera]),
          "published full PNGs retain their exact file content");
      const cv::Mat thumbnail = cv::imread(inputs.thumbnails[index][camera].string(), cv::IMREAD_UNCHANGED);
      ok &= expect(
          !thumbnail.empty() && thumbnail.depth() == CV_8U && std::max(thumbnail.cols, thumbnail.rows) <= 1024,
          "inspection JPEGs are bounded eight-bit images");
      if (!thumbnail.empty() && index == 1) {
        const cv::Scalar average = cv::mean(thumbnail);
        ok &= expect(
            average[0] < 30 && average[1] > 50 && average[1] < 80 && average[2] > 110 && average[2] < 145,
            "sixteen-bit thumbnails scale their range instead of saturating white");
      }
    }
  }
  const fs::path manifest = bundle_path(source, plan) / "frames.yaml";
  const auto original_manifest = read_bytes(manifest);
  const auto original_timestamp = fs::last_write_time(manifest);
  ok &= expect(PublishPlayerFrameInputs(source, plan, images).ok(), "publishing identical input is idempotent");
  ok &= expect(
      read_bytes(manifest) == original_manifest && fs::last_write_time(manifest) == original_timestamp,
      "idempotent publication leaves the immutable manifest untouched");
  ok &= expect(CopyPlayerFrameInputs(source, destination, plan).ok(), "copy a complete bundle into another game");
  auto copied = LoadPlayerFrameInputs(destination, plan);
  if (!expect(copied.ok() && copied->has_value(), "copied bundle validates independently"))
    return false;
  for (size_t index = 0; index < 2; ++index) {
    for (size_t camera = 0; camera < 2; ++camera) {
      ok &= expect(
          read_bytes((**copied).images[index][camera]) == read_bytes(inputs.images[index][camera]) &&
              read_bytes((**copied).thumbnails[index][camera]) == read_bytes(inputs.thumbnails[index][camera]),
          "copy preserves exact full input and thumbnail bytes");
    }
  }

  const fs::path corrupted = root / "corrupt-png";
  clone_bundle(source, corrupted);
  const fs::path damaged_png = bundle_path(corrupted, plan) / "left_0.png";
  cv::Mat modified = cv::imread(damaged_png.string(), cv::IMREAD_UNCHANGED);
  modified.at<cv::Vec3b>(10, 10)[0] ^= 1;
  const auto previous_size = fs::file_size(damaged_png);
  if (!cv::imwrite(damaged_png.string(), modified, {cv::IMWRITE_PNG_COMPRESSION, 0}))
    return false;
  ok &= expect(fs::file_size(damaged_png) == previous_size, "content-corruption fixture retains the PNG byte count");
  ok &= expect(!LoadPlayerFrameInputs(corrupted, plan).ok(), "full validation rejects same-size changed PNG content");
  auto inspection = LoadPlayerFrameInputs(corrupted, plan, PlayerFrameInputValidation::kInspection);
  ok &= expect(
      inspection.ok() && inspection->has_value(), "inspection does not hash full PNG content on its thumbnail path");
  const fs::path failed_copy = root / "failed-copy";
  fs::create_directories(failed_copy);
  ok &= expect(!CopyPlayerFrameInputs(corrupted, failed_copy, plan).ok(), "copy rejects a corrupted source bundle");
  auto not_published = LoadPlayerFrameInputs(failed_copy, plan);
  ok &= expect(
      not_published.ok() && !not_published->has_value(), "failed copy never advertises a partially published bundle");
  ok &= expect(
      !PublishPlayerFrameInputs(corrupted, plan, images).ok(), "publication never repairs a present corrupt bundle");
  auto conflicting_images = images;
  conflicting_images[0][0] = damaged_png;
  ok &= expect(
      !PublishPlayerFrameInputs(source, plan, conflicting_images).ok(),
      "a second publication cannot silently accept different pixels for the same frozen plan");
  ok &= expect(
      read_bytes(inputs.images[0][0]) == read_bytes(images[0][0]),
      "conflicting publication preserves the originally saved input");

  const auto rejects = [&](const std::string& name, const auto& mutate) {
    const fs::path game = root / name;
    clone_bundle(source, game);
    mutate(bundle_path(game, plan));
    return expect(!LoadPlayerFrameInputs(game, plan).ok(), name + " is an error, never legacy absence");
  };
  ok &= rejects("missing-manifest", [](const fs::path& bundle) { fs::remove(bundle / "frames.yaml"); });
  ok &= rejects("invalid-yaml", [](const fs::path& bundle) { write_bytes(bundle / "frames.yaml", "[unterminated"); });
  ok &= rejects("partial-pairs", [](const fs::path& bundle) { fs::remove(bundle / "right_1.png"); });
  ok &= rejects("missing-thumbnail", [](const fs::path& bundle) { fs::remove(bundle / "left_1.jpg"); });
  ok &= rejects("wrong-fingerprint", [&](const fs::path& bundle) {
    auto text = read_bytes(bundle / "frames.yaml");
    const size_t start = text.find(plan.fingerprint);
    if (start == std::string::npos)
      throw std::runtime_error("Published manifest is missing its fingerprint");
    text[start] = text[start] == '0' ? '1' : '0';
    write_bytes(bundle / "frames.yaml", text);
  });
  ok &= rejects("oversized-manifest", [](const fs::path& bundle) {
    write_bytes(bundle / "frames.yaml", std::string(256 * 1024 + 1, ' '));
  });
  ok &= rejects("oversized-png", [](const fs::path& bundle) {
    fs::resize_file(bundle / "left_0.png", 512ULL * 1024 * 1024 + 1);
  });
  ok &= rejects(
      "oversized-jpeg", [](const fs::path& bundle) { fs::resize_file(bundle / "right_1.jpg", 4 * 1024 * 1024 + 1); });
  ok &= rejects("excessive-png-dimension", [](const fs::path& bundle) {
    auto bytes = read_bytes(bundle / "left_0.png");
    // IHDR width 32769 must be rejected before an image decoder allocates pixels.
    bytes[16] = 0;
    bytes[17] = 0;
    bytes[18] = static_cast<char>(0x80);
    bytes[19] = 1;
    write_bytes(bundle / "left_0.png", bytes);
  });
  ok &= rejects("unsupported-version", [](const fs::path& bundle) {
    auto node = YAML::Load(read_bytes(bundle / "frames.yaml"));
    node["version"] = 99;
    write_bytes(bundle / "frames.yaml", YAML::Dump(node));
  });
  ok &= rejects("manifest-pair-count", [](const fs::path& bundle) {
    auto node = YAML::Load(read_bytes(bundle / "frames.yaml"));
    node["pairs"].remove(static_cast<size_t>(1));
    write_bytes(bundle / "frames.yaml", YAML::Dump(node));
  });
  ok &= rejects("wrong-embedded-plan", [&](const fs::path& bundle) {
    auto other = plan;
    ++other.selected[1].pair.cameras[1].source_pts_ns;
    const auto fingerprint = PlayerFrameSelectionFingerprint(other);
    if (!fingerprint.ok())
      throw std::runtime_error(fingerprint.status().ToString());
    other.fingerprint = *fingerprint;
    auto node = YAML::Load(read_bytes(bundle / "frames.yaml"));
    node["plan"] = PlayerFrameSelectionPlanYaml(other);
    write_bytes(bundle / "frames.yaml", YAML::Dump(node));
  });
  ok &= rejects("symlink-image", [&](const fs::path& bundle) {
    fs::remove(bundle / "left_0.png");
    fs::create_symlink(images[0][0], bundle / "left_0.png");
  });
  ok &= rejects("symlink-manifest", [&](const fs::path& bundle) {
    fs::remove(bundle / "frames.yaml");
    fs::create_symlink(manifest, bundle / "frames.yaml");
  });
  ok &= rejects("directory-image", [](const fs::path& bundle) {
    fs::remove(bundle / "right_0.png");
    fs::create_directory(bundle / "right_0.png");
  });
  ok &= rejects("fifo-image", [](const fs::path& bundle) {
    const fs::path image = bundle / "right_0.png";
    fs::remove(image);
    if (::mkfifo(image.c_str(), 0600) != 0)
      throw std::runtime_error("Cannot create nonblocking read fixture");
  });

  const fs::path bad_thumbnail = root / "corrupt-thumbnail";
  clone_bundle(source, bad_thumbnail);
  const fs::path thumbnail_path = bundle_path(bad_thumbnail, plan) / "right_1.jpg";
  auto thumbnail_bytes = read_bytes(thumbnail_path);
  thumbnail_bytes[thumbnail_bytes.size() / 2] ^= 1;
  write_bytes(thumbnail_path, thumbnail_bytes);
  ok &= expect(
      !LoadPlayerFrameInputs(bad_thumbnail, plan, PlayerFrameInputValidation::kInspection).ok(),
      "inspection still verifies thumbnail content digests");
  fs::remove(thumbnail_path);
  fs::create_symlink(inputs.thumbnails[1][1], thumbnail_path);
  ok &= expect(
      !LoadPlayerFrameInputs(bad_thumbnail, plan, PlayerFrameInputValidation::kInspection).ok(),
      "inspection rejects a thumbnail symlink even when its target bytes are valid");

  const fs::path symlink_store = root / "symlink-store";
  fs::create_directories(symlink_store);
  fs::create_directory_symlink(source / "player-frame-inputs", symlink_store / "player-frame-inputs");
  ok &= expect(!LoadPlayerFrameInputs(symlink_store, plan).ok(), "store parent cannot redirect through a symlink");
  ok &= expect(!PublishPlayerFrameInputs(symlink_store, plan, images).ok(), "publication rejects a symlink store");
  const fs::path redirected = root / "unowned-target";
  fs::create_directory(redirected);
  fs::remove(symlink_store / "player-frame-inputs");
  fs::create_directory_symlink(redirected, symlink_store / "player-frame-inputs");
  ok &= expect(
      !PublishPlayerFrameInputs(symlink_store, plan, images).ok() && fs::is_empty(redirected),
      "rejecting a redirected store must not create a lock or any other file in its target");
  const fs::path symlink_bundle = root / "symlink-bundle";
  fs::create_directories(symlink_bundle / "player-frame-inputs");
  fs::create_directory_symlink(bundle_path(source, plan), bundle_path(symlink_bundle, plan));
  ok &= expect(!LoadPlayerFrameInputs(symlink_bundle, plan).ok(), "bundle directory cannot redirect through a symlink");

  const fs::path incomplete = root / "failed-publish";
  fs::create_directories(incomplete);
  auto incomplete_images = images;
  incomplete_images.back()[1] = root / "never-captured.png";
  ok &= expect(
      !PublishPlayerFrameInputs(incomplete, plan, incomplete_images).ok(), "publication requires every captured image");
  auto incomplete_load = LoadPlayerFrameInputs(incomplete, plan);
  ok &= expect(
      incomplete_load.ok() && !incomplete_load->has_value(), "failed publication leaves no visible partial bundle");
  incomplete_images.pop_back();
  ok &= expect(
      !PublishPlayerFrameInputs(incomplete, plan, incomplete_images).ok(), "published pair count must match the plan");

  auto changed_plan = plan;
  ++changed_plan.selected[1].pair.cameras[0].source_pts_ns;
  ok &=
      expect(!LoadPlayerFrameInputs(source, changed_plan).ok(), "bundle load validates the complete plan fingerprint");
  ok &= expect(
      !PublishPlayerFrameInputs(incomplete, changed_plan, images).ok(), "publication rejects a mutated frozen plan");

  return ok;
}

} // namespace

int main() {
  std::string pattern = (fs::temp_directory_path() / "player-input-store-test-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const char* directory = ::mkdtemp(writable.data());
  if (!directory) {
    std::cerr << "Cannot create input-store test directory\n";
    return 1;
  }
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup{directory};
  try {
    return run(cleanup.path) ? 0 : 1;
  } catch (const std::exception& exception) {
    std::cerr << "Input-store test fixture failed: " << exception.what() << '\n';
    return 1;
  }
}
