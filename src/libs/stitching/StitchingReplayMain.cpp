#include "hstream/src/libs/stitching/HuginProject.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace stitching = hm::stitching;

void merge(YAML::Node destination, const YAML::Node& overlay) {
  for (const auto& entry : overlay) {
    const auto key = entry.first.as<std::string>();
    if (entry.second.IsMap() && destination[key].IsMap())
      merge(destination[key], entry.second);
    else
      destination[key] = YAML::Clone(entry.second);
  }
}

template <class T>
T checked(absl::StatusOr<T> value) {
  if (!value.ok())
    throw std::runtime_error(value.status().ToString());
  return std::move(*value);
}

// Replay the original two-camera tie points through the production calibration
// and publication path. A fresh destination keeps review renders independent of
// a playing game's maps, ROI coordinates and generation claims.
void replay(const fs::path& source, const fs::path& destination, const fs::path& preset) {
  if (fs::exists(destination))
    throw std::runtime_error("Output game directory must not already exist");
  auto lock = checked(stitching::HuginProject::RecoverAndLock(source));
  auto source_config = checked(stitching::load_game_config_file(source / "config.yaml"));
  if (!source_config.has_value())
    throw std::runtime_error("Source game has no config.yaml");
  auto config = YAML::Clone(*source_config);
  const auto overlay = YAML::LoadFile(preset.string());
  if (!overlay.IsMap())
    throw std::runtime_error("Preset must be a YAML map");
  merge(config, overlay);
  config.remove("hstream_ui");
  config.remove("rink"); // Source panorama coordinates do not belong to this view.
  config["stitching"].remove("generated_field_mask_post_stitch_rotate_degrees");
  config["stitching"]["mapping_backend"] = "nona";
  config["stitching"]["run_autooptimizer"] = true;
  config["stitching"]["post_stitch_rotate_degrees"] = 0;
  for (const char* side : {"left", "right"}) {
    auto videos = config["game"]["videos"][side];
    if (!videos.IsSequence())
      throw std::runtime_error("Source game must declare left and right game.videos sequences");
    for (size_t index = 0; index < videos.size(); ++index)
      videos[index] = fs::absolute(source / videos[index].as<std::string>()).string();
  }

  stitching::HuginProject::Options options;
  options.mapping_backend = stitching::MappingBackend::kNona;
  options.run_autooptimizer = true;
  options.control_point_matcher = checked(
      stitching::ParseControlPointMatcher(
          config["stitching"]["control_point_matcher"].as<std::string>("superpoint-lightglue")));
  options.projection = checked(stitching::ParseStitchProjection(config["stitching"]["projection"].as<std::string>()));
  options.projection_parameters = checked(stitching::read_stitch_projection_parameters(config, *options.projection));
  options.projection_framing = checked(stitching::read_stitch_projection_framing(config));
  const auto camera = checked(stitching::read_stitch_camera_selection(config));
  options.camera_configuration = camera.configuration;
  options.horizontal_fov = camera.horizontal_fov;
  options.vertical_fov = camera.vertical_fov;
  options.max_output_width = config["stitching"]["max_output_width"].as<size_t>(3840);
  config["stitching"]["max_output_width"] = *options.max_output_width;

  std::ifstream project(source / "hm_project.pto");
  if (!project)
    throw std::runtime_error("Cannot read source hm_project.pto");
  std::vector<stitching::FeatureMatch> matches;
  bool camera_checked = false;
  for (std::string line; std::getline(project, line);) {
    if (line.rfind("i ", 0) == 0 && !camera_checked) {
      std::istringstream tokens(line);
      for (std::string token; tokens >> token;) {
        if (token[0] == 'v' && token.size() > 1 && token[1] != '=') {
          if (std::abs(std::stod(token.substr(1)) - options.horizontal_fov) > 1e-6)
            throw std::runtime_error("Preset camera FOV differs from the saved calibration's input FOV");
          camera_checked = true;
        }
      }
    }
    if (line.rfind("c ", 0) != 0)
      continue;
    std::istringstream tokens(line);
    char prefix;
    int left = -1, right = -1, type = -1;
    double x = NAN, y = NAN, X = NAN, Y = NAN;
    tokens >> prefix;
    while (tokens >> prefix) {
      switch (prefix) {
        case 'n':
          tokens >> left;
          break;
        case 'N':
          tokens >> right;
          break;
        case 'x':
          tokens >> x;
          break;
        case 'y':
          tokens >> y;
          break;
        case 'X':
          tokens >> X;
          break;
        case 'Y':
          tokens >> Y;
          break;
        case 't':
          tokens >> type;
          break;
        default:
          throw std::runtime_error("Unsupported saved control-point token");
      }
    }
    if (left != 0 || right != 1 || type != 0 || !std::isfinite(x + y + X + Y))
      throw std::runtime_error("Replay requires finite two-camera point correspondences (n0 N1 t0)");
    matches.push_back({cv::Point2f(x, y), cv::Point2f(X, Y), 1.0f});
  }
  if (!camera_checked || matches.size() < 16)
    throw std::runtime_error("Saved calibration has no camera FOV or insufficient control points");
  if (!fs::create_directories(destination))
    throw std::runtime_error("Output game directory was created by another process");
  std::ofstream output_config(destination / "config.yaml");
  output_config << config << '\n';
  output_config.close();
  if (!output_config)
    throw std::runtime_error("Cannot write output config.yaml");
  const auto status =
      stitching::HuginProject::Configure(destination, source / "left.png", source / "right.png", matches, options);
  if (!status.ok())
    throw std::runtime_error(status.ToString());
  std::cout << "Replayed " << matches.size() << " saved control points into " << destination << '\n';
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: stitching-replay SOURCE_GAME NEW_OUTPUT_GAME PRESET.yaml\n";
    return 2;
  }
  try {
    replay(fs::absolute(argv[1]), fs::absolute(argv[2]), fs::absolute(argv[3]));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
