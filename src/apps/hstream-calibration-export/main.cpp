#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <sys/stat.h>
#include <yaml-cpp/yaml.h>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/UserConfig.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/FieldMaskArtifact.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/LiveOutputEpoch.h"
#include "hstream/src/libs/stitching/LiveStitchingGeneration.h"
#include "hstream/src/libs/stitching/PlaybackRinkMap.h"
#include "hstream/src/libs/stitching/ScoreboardSelector.h"

namespace {
namespace fs = std::filesystem;
namespace stitching = hm::stitching;

void check(const absl::Status& status) {
  if (!status.ok())
    throw std::runtime_error(status.ToString());
}

template <class T>
T take(absl::StatusOr<T> value) {
  if (!value.ok())
    throw std::runtime_error(value.status().ToString());
  return std::move(*value);
}

YAML::Node merge(const YAML::Node& base, const YAML::Node& overlay) {
  if (!overlay.IsMap())
    return YAML::Clone(overlay);
  YAML::Node result = base.IsMap() ? YAML::Clone(base) : YAML::Node(YAML::NodeType::Map);
  for (const auto& entry : overlay) {
    const auto key = entry.first.as<std::string>();
    result[key] = merge(result[key], entry.second);
  }
  return result;
}

std::string digest(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("Cannot read exported artifact: " + path.string());
  EVP_MD_CTX* raw = EVP_MD_CTX_new();
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw, EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("Cannot initialize snapshot checksum");
  std::vector<char> buffer(1024 * 1024);
  while (input) {
    input.read(buffer.data(), buffer.size());
    if (input.gcount() && EVP_DigestUpdate(context.get(), buffer.data(), input.gcount()) != 1)
      throw std::runtime_error("Cannot checksum snapshot");
  }
  if (!input.eof())
    throw std::runtime_error("Cannot finish reading exported artifact: " + path.string());
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned length = 0;
  if (EVP_DigestFinal_ex(context.get(), bytes, &length) != 1 || length != 32)
    throw std::runtime_error("Cannot finish snapshot checksum");
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned i = 0; i < length; ++i)
    output << std::setw(2) << static_cast<unsigned>(bytes[i]);
  return output.str();
}

YAML::Node identity(const fs::path& path) {
  struct stat info{};
  if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0)
    throw std::runtime_error("Source recording is not a nonempty regular file: " + path.string());
  YAML::Node result;
  result["path"] = fs::canonical(path).string();
  result["device"] = static_cast<uint64_t>(info.st_dev);
  result["inode"] = static_cast<uint64_t>(info.st_ino);
  result["size"] = static_cast<uint64_t>(info.st_size);
  result["mtime_ns"] = static_cast<int64_t>(info.st_mtim.tv_sec) * 1000000000LL + info.st_mtim.tv_nsec;
  return result;
}

YAML::Node source_identity(const fs::path& game, const YAML::Node& config) {
  const auto videos = config["game"]["videos"];
  if (!videos.IsMap())
    throw std::runtime_error("Native calibration requires game.videos");
  YAML::Node result;
  for (const char* role : {"left", "right"}) {
    const auto chapters = videos[role];
    if (!chapters.IsSequence() || chapters.size() == 0)
      throw std::runtime_error(std::string("Native calibration requires ordered ") + role + " chapters");
    for (const auto& chapter : chapters) {
      const fs::path path(chapter.as<std::string>());
      result[role].push_back(identity(path.is_absolute() ? path : game / path));
    }
  }
  return result;
}

void export_calibration(
    const fs::path& game,
    const fs::path& destination,
    size_t maximum_width,
    const fs::path& session_config) {
  // Recover and validate with the same implementation that loads the native
  // renderer. Keep its artifact lock through the complete private copy.
  auto artifacts = take(stitching::lock_stitching_artifacts_for_load(game.string(), maximum_width));
  if (!artifacts.artifact_lock || !artifacts.load_snapshot)
    throw std::runtime_error("Native stitching calibration is missing or incompatible; calibrate in hstream-ui first");
  auto config_lock = take(stitching::GameConfigTransactionLock::Acquire(game));
  const YAML::Node public_config = YAML::LoadFile((game / "config.yaml").string());
  const YAML::Node calibration = public_config["hstream_ui"]["stitching_calibration"];
  if (!calibration.IsMap() || !calibration["status"] || calibration["status"].as<std::string>() != "complete" ||
      !calibration["invalidation_id"])
    throw std::runtime_error("Native calibration must be complete before starting HockeyMON");
  const auto owner = calibration["invalidation_id"].as<std::string>();
  if (owner.empty())
    throw std::runtime_error("Native calibration has no generation owner");
  auto baseline = take(hm::baseline_config::load());
  auto effective = merge(baseline.values, take(hm::user_config::load_or_create()));
  effective = merge(effective, public_config);
  if (!session_config.empty()) {
    if (fs::file_size(session_config) > 16 * 1024 * 1024)
      throw std::runtime_error("Session configuration exceeds the metadata limit");
    const auto session = YAML::LoadFile(session_config.string());
    if (!session.IsMap())
      throw std::runtime_error("Session configuration must be a mapping");
    effective = merge(effective, session);
  }
  const auto choices = take(stitching::read_stitching_backend_choices(effective));
  check(stitching::validate_stitching_backend_generation(public_config, owner, choices));
  const auto sources = source_identity(game, effective);
  const auto game_stitching = public_config["game"]["stitching"];
  YAML::Node offsets = game_stitching && game_stitching.IsMap() ? game_stitching["frame_offsets"]
                                                                : YAML::Node(YAML::NodeType::Undefined);
  if (!offsets.IsMap())
    offsets = public_config["stitching"]["frame_offsets"];
  if (!offsets.IsMap())
    throw std::runtime_error("Native calibration has no published synchronization offsets");
  effective["game"]["stitching"]["frame_offsets"] = YAML::Clone(offsets);
  for (const char* role : {"left", "right"}) {
    if (!offsets[role] || !std::isfinite(offsets[role].as<double>()) || offsets[role].as<double>() < 0)
      throw std::runtime_error("Native calibration has missing or invalid synchronization offsets");
  }
  if (!fs::create_directory(destination))
    throw std::runtime_error("Export destination must not already exist: " + destination.string());
  fs::permissions(destination, fs::perms::owner_all);
  struct RemoveIncomplete {
    fs::path path;
    bool complete{false};
    ~RemoveIncomplete() {
      if (!complete) {
        std::error_code error;
        fs::remove_all(path, error);
        if (error)
          std::cerr << "Cannot remove incomplete calibration export " << path << ": " << error.message() << '\n';
      }
    }
  } cleanup{destination};
  YAML::Node manifest;
  manifest["version"] = 1;
  manifest["game_directory"] = fs::canonical(game).string();
  manifest["generation"] = artifacts.generation_id;
  manifest["invalidation_id"] = owner;
  const auto output_generation = take(
      stitching::current_stitched_output_generation_id_locked(game.string(), public_config, artifacts.generation_id));
  manifest["output_generation"] = output_generation;
  stitching::PlaybackRinkMap output_identity;
  output_identity.hugin_generation = artifacts.generation_id;
  const auto rotation =
      output_identity.OutputRotation(output_generation, artifacts.canvas_size.width, artifacts.canvas_size.height);
  if (!rotation.has_value())
    throw std::runtime_error("Native output generation does not match its exported canvas");
  manifest["post_stitch_rotate_degrees"] = *rotation;
  effective["stitching"]["post_stitch_rotate_degrees"] = *rotation;
  manifest["canvas"]["width"] = artifacts.canvas_size.width;
  manifest["canvas"]["height"] = artifacts.canvas_size.height;
  manifest["max_output_width"] = maximum_width;
  manifest["frame_offsets"] = YAML::Clone(offsets);
  manifest["sources"] = sources;
  manifest["configuration"] = effective;
  const auto original_identity = take(stitching::stitch_artifact_content_identity_locked(game));
  for (const std::string& name : stitching::stitch_artifact_names()) {
    const auto source = game / name;
    if (!fs::exists(source))
      continue;
    check(stitching::clone_or_copy_stitch_rollback_file(source, destination / name));
    manifest["artifacts"][name]["size"] = fs::file_size(destination / name);
    manifest["artifacts"][name]["sha256"] = digest(destination / name);
  }
  const auto copied_identity = take(stitching::stitch_artifact_content_identity_locked(destination));
  if (copied_identity.generation_id != artifacts.generation_id ||
      copied_identity.fingerprint != original_identity.fingerprint)
    throw std::runtime_error("Native calibration changed while it was exported");
  check(artifacts.load_snapshot->verify());
  if (YAML::Dump(source_identity(game, effective)) != YAML::Dump(sources))
    throw std::runtime_error("Source recordings changed while calibration was exported");
  // The field-mask visitor takes the same locks itself. Releasing here is
  // safe: its explicit owner/output generation rejects a concurrent replacement.
  config_lock.reset();
  artifacts.artifact_lock.reset();
  check(stitching::visit_current_field_mask(game.string(), output_generation, owner, [&](const std::string& path) {
    return stitching::clone_or_copy_stitch_rollback_file(path, destination / "rink_mask_0.png");
  }));
  manifest["artifacts"]["rink_mask_0.png"]["size"] = fs::file_size(destination / "rink_mask_0.png");
  manifest["artifacts"]["rink_mask_0.png"]["sha256"] = digest(destination / "rink_mask_0.png");
  // Store a private config so Python's downstream cache generation cannot
  // write to the shared game's native calibration files.
  check(stitching::publish_named_file(destination / "config.yaml", YAML::Dump(effective) + "\n"));
  manifest["artifacts"]["config.yaml"]["size"] = fs::file_size(destination / "config.yaml");
  manifest["artifacts"]["config.yaml"]["sha256"] = digest(destination / "config.yaml");
  check(stitching::publish_named_file(destination / "native-calibration.yaml", YAML::Dump(manifest) + "\n"));
  cleanup.complete = true;
}
YAML::Node scoreboard_canvas(
    const fs::path& game,
    const fs::path& image_destination,
    const fs::path& source_reference) {
  const auto canvas =
      take(stitching::ScoreboardSelector::OpenCanvas(source_reference.empty() ? game : source_reference, true));
  if (!source_reference.empty() && !canvas.generation.hugin_generation.empty())
    throw std::runtime_error("Private scoreboard reference must belong to a single source");
  check(stitching::publish_named_file(image_destination, canvas.png_bytes));
  YAML::Node result;
  result["game_directory"] = game.string();
  if (!source_reference.empty()) {
    result["source_game_directory"] = source_reference.string();
    result["persist_to_game"] = take(stitching::ScoreboardSelector::SameSingleSource(game, source_reference));
  }
  result["width"] = canvas.width;
  result["height"] = canvas.height;
  result["image_path"] = image_destination.string();
  result["expected_generation"]["hugin_generation"] = canvas.generation.hugin_generation;
  result["expected_generation"]["stitched_output_generation"] = canvas.generation.stitched_output_generation;
  result["expected_generation"]["snapshot_identity"] = canvas.generation.snapshot_identity;
  return result;
}

YAML::Node save_scoreboard(const fs::path& game, const fs::path& request_path) {
  if (fs::file_size(request_path) > 16384)
    throw std::runtime_error("Scoreboard request exceeds the metadata limit");
  const YAML::Node request = YAML::LoadFile(request_path.string());
  const YAML::Node points = request["points"];
  if (!points.IsSequence() || points.size() != 4)
    throw std::runtime_error("Scoreboard requires four points");
  stitching::ScoreboardSelector::Polygon polygon;
  for (size_t index = 0; index < polygon.size(); ++index) {
    if (!points[index].IsSequence() || points[index].size() != 2)
      throw std::runtime_error("Scoreboard point requires x and y");
    polygon[index] = {points[index][0].as<int>(), points[index][1].as<int>()};
  }
  const auto canvas = request["canvas"];
  const auto generation = canvas["expected_generation"];
  const stitching::ScoreboardSelector::CanvasGeneration expected{
      generation["hugin_generation"].as<std::string>(),
      generation["stitched_output_generation"].as<std::string>(),
      generation["snapshot_identity"].as<std::string>()};
  if (expected.snapshot_identity.empty())
    throw std::runtime_error("Scoreboard requires a pinned native canvas generation");
  const fs::path source_reference =
      canvas["source_game_directory"] ? fs::canonical(canvas["source_game_directory"].as<std::string>()) : fs::path{};
  if (!source_reference.empty() &&
      (!expected.hugin_generation.empty() || canvas["game_directory"].as<std::string>() != game.string()))
    throw std::runtime_error("Private scoreboard reference belongs to a different source or game");
  const auto current_canvas =
      take(stitching::ScoreboardSelector::OpenCanvas(source_reference.empty() ? game : source_reference, true));
  if (current_canvas.width != canvas["width"].as<int>() || current_canvas.height != canvas["height"].as<int>() ||
      current_canvas.generation.hugin_generation != expected.hugin_generation ||
      current_canvas.generation.stitched_output_generation != expected.stitched_output_generation ||
      current_canvas.generation.snapshot_identity != expected.snapshot_identity)
    throw std::runtime_error("Scoreboard canvas changed; reopen the selector");
  if (!stitching::ScoreboardSelector::IsDisabled(polygon))
    polygon = take(
        stitching::ScoreboardSelector::ValidateAndOrder(
            polygon, canvas["width"].as<int>(), canvas["height"].as<int>()));
  if (source_reference.empty()) {
    check(stitching::ScoreboardSelector::Save(game, polygon, expected));
  } else if (canvas["persist_to_game"].as<bool>()) {
    check(stitching::ScoreboardSelector::Save(game, polygon, expected, source_reference));
  } else {
    check(stitching::ScoreboardSelector::Save(source_reference, polygon, expected));
  }
  YAML::Node result;
  for (const auto& point : polygon) {
    YAML::Node pair;
    pair.push_back(point.x);
    pair.push_back(point.y);
    result["points"].push_back(pair);
  }
  return result;
}
YAML::Node validate_epoch(const fs::path& game, const fs::path& request_path) {
  if (fs::file_size(request_path) > 16384)
    throw std::runtime_error("Live output epoch request exceeds the metadata limit");
  const YAML::Node request = YAML::LoadFile(request_path.string());
  const auto epoch = take(stitching::parse_live_output_epoch(request["serialized_epoch"].as<std::string>()));
  std::string output_generation;
  check(
      stitching::validate_live_output_epoch(
          game.string(), epoch, request["loaded_hugin_generation"].as<std::string>(), &output_generation));
  YAML::Node result;
  result["post_stitch_rotate_degrees"] = epoch.post_stitch_rotate_degrees;
  result["output_generation"] = output_generation;
  result["authorization_id"] = epoch.authorization_id;
  result["scoreboard_property_value"] = epoch.scoreboard_property_value;
  return result;
}
YAML::Node export_rink_map(const fs::path& game, const fs::path& request_path) {
  if (fs::file_size(request_path) > 16384)
    throw std::runtime_error("Rink map request exceeds the metadata limit");
  const YAML::Node request = YAML::LoadFile(request_path.string());
  const auto map = take(stitching::LoadPlaybackRinkMap(game));
  if (map->hugin_generation != request["loaded_hugin_generation"].as<std::string>())
    throw std::runtime_error("Rink map belongs to a different loaded calibration");
  const auto width = request["width"].as<unsigned>();
  const auto height = request["height"].as<unsigned>();
  const auto rotation = map->OutputRotation(request["output_generation"].as<std::string>(), width, height);
  if (!rotation.has_value())
    throw std::runtime_error("Rink map does not match the loaded output generation");
  YAML::Node result;
  result["version"] = 1;
  result["hugin_generation"] = map->hugin_generation;
  result["output_generation"] = request["output_generation"];
  result["width"] = width;
  result["height"] = height;
  result["rotation_degrees"] = *rotation;
  result["layout"]["length"] = map->layout.length;
  result["layout"]["width"] = map->layout.width;
  result["layout"]["blue_line_distance"] = map->layout.blue_line_distance;
  result["layout"]["goal_inset"] = map->layout.goal_inset;
  result["layout"]["corner_radius"] = map->layout.corner_radius;
  result["fit"]["state"] = "ready";
  result["fit"]["rms_degrees"] = map->fit.rms_degrees;
  result["fit"]["constraints"] = map->fit.constraints;
  for (size_t row = 0; row < 3; ++row) {
    YAML::Node values;
    for (size_t column = 0; column < 3; ++column)
      values.push_back(map->fit.plane_to_ray(row, column));
    result["fit"]["plane_to_ray"].push_back(values);
  }
  result["columns"] = map->columns;
  result["rows"] = map->rows;
  for (const auto& ray : map->rays) {
    YAML::Node values;
    for (int axis = 0; axis < 3; ++axis)
      values.push_back(ray[axis]);
    result["rays"].push_back(values);
  }
  return result;
}
} // namespace

int main(int argc, char** argv) {
  try {
    fs::path game, destination, scoreboard_request, scoreboard_image, epoch_request, rink_map_request, session_config;
    fs::path scoreboard_reference;
    size_t maximum_width = 0;
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--help") {
        std::cout << "Usage: hstream-calibration-export --game-dir DIR\n"
                     "  --destination NEW_DIR [--max-output-width N] [--config YAML]\n"
                     "  --scoreboard-canvas PRIVATE_PNG [--scoreboard-reference-game PRIVATE_DIR]\n"
                     "  --save-scoreboard REQUEST_YAML\n"
                     "  --validate-epoch REQUEST_YAML\n"
                     "  --rink-map REQUEST_YAML\n";
        return 0;
      }
      if (++i == argc)
        throw std::runtime_error("Missing value for " + argument);
      if (argument == "--game-dir")
        game = argv[i];
      else if (argument == "--destination")
        destination = argv[i];
      else if (argument == "--config")
        session_config = argv[i];
      else if (argument == "--rink-map")
        rink_map_request = argv[i];
      else if (argument == "--validate-epoch")
        epoch_request = argv[i];
      else if (argument == "--save-scoreboard")
        scoreboard_request = argv[i];
      else if (argument == "--scoreboard-reference-game")
        scoreboard_reference = fs::canonical(argv[i]);
      else if (argument == "--scoreboard-canvas")
        scoreboard_image = argv[i];
      else if (argument == "--max-output-width") {
        const std::string value = argv[i];
        size_t consumed = 0;
        if (value.empty() || value.front() == '-')
          throw std::runtime_error("Invalid maximum output width");
        maximum_width = std::stoull(value, &consumed);
        if (consumed != value.size() || maximum_width > 65536)
          throw std::runtime_error("Invalid maximum output width");
      } else
        throw std::runtime_error("Unknown option: " + argument);
    }
    if (game.empty())
      throw std::runtime_error("Game directory is required");
    const int modes = !destination.empty() + !scoreboard_request.empty() + !scoreboard_image.empty() +
        !epoch_request.empty() + !rink_map_request.empty();
    if (modes != 1)
      throw std::runtime_error(
          "Specify exactly one export destination, scoreboard operation, or epoch validation request");
    if (!scoreboard_reference.empty() && scoreboard_image.empty())
      throw std::runtime_error("A private scoreboard reference requires --scoreboard-canvas");
    if (!rink_map_request.empty()) {
      std::cout << export_rink_map(fs::canonical(game), rink_map_request) << '\n';
      return 0;
    }
    if (!epoch_request.empty()) {
      std::cout << validate_epoch(fs::canonical(game), epoch_request) << '\n';
      return 0;
    }
    if (!scoreboard_image.empty()) {
      std::cout << scoreboard_canvas(fs::canonical(game), fs::absolute(scoreboard_image), scoreboard_reference) << '\n';
      return 0;
    }
    if (!scoreboard_request.empty()) {
      std::cout << save_scoreboard(fs::canonical(game), scoreboard_request) << '\n';
      return 0;
    }
    if (game.empty() || destination.empty())
      throw std::runtime_error("Game directory and export destination are required");
    export_calibration(fs::canonical(game), fs::absolute(destination), maximum_width, session_config);
    std::cout << "Exported native calibration to " << destination.string() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "hstream-calibration-export: " << error.what() << '\n';
    return 1;
  }
}
