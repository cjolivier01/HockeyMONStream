#include "hstream/src/libs/playtracker_replay/ReplaySession.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

// Explicit offline comparison: metadata only, with no video decoding or changes
// to the recording/game. CSV rows align both views to the same source sample.
int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr
        << "Usage: rink_camera_comparison RECORDING.db GAME_DIRECTORY START_SECONDS DURATION_SECONDS OUTPUT.csv\n";
    return 1;
  }
  try {
    if (std::filesystem::exists(argv[5]))
      throw std::runtime_error("Comparison output already exists");
    hm::playtracker_replay::PrepareOptions options;
    options.manifest_path = argv[1];
    options.start_seconds = std::stod(argv[3]);
    options.duration_seconds = std::stod(argv[4]);
    const auto session = hm::playtracker_replay::ReplaySession::Prepare(options);
    if (!session.ok())
      throw std::runtime_error(session.status().ToString());
    auto map = hm::stitching::LoadPlaybackRinkMap(argv[2]);
    if (!map.ok())
      throw std::runtime_error(map.status().ToString());
    hm::playtracker_replay::RinkCameraSetup setup{*map, {}};
    DsPlayTrackerRuntimeTuning tuning;
    tuning.update_motion_tuning = false;
    const auto native_start = std::chrono::steady_clock::now();
    const auto native = (*session)->RunTrial("Native", tuning);
    if (!native.ok())
      throw std::runtime_error(native.status().ToString());
    const auto guide_start = std::chrono::steady_clock::now();
    const auto trial = (*session)->RunTrial("Rink-aware", tuning, nullptr, &setup);
    const auto finished = std::chrono::steady_clock::now();
    if (!trial.ok())
      throw std::runtime_error(trial.status().ToString());
    std::ostringstream out;
    out << std::setprecision(12);
    out << "sample,seconds,baseline_left,baseline_top,baseline_right,baseline_bottom,"
        << "rink_left,rink_top,rink_right,rink_bottom,speed_mps,lead_pixels,width_ratio,goal,located,active,baseline_goal_visible,rink_goal_visible\n";
    double native_travel = 0, rink_travel = 0, width_ratio = 0, max_speed = 0;
    size_t compared = 0, active = 0, goals = 0;
    size_t baseline_goal_visible = 0, rink_goal_visible = 0;
    const double rotation = *(*session)->RinkCameraRotation(**map);
    std::array<std::optional<cv::Point2d>, 2> goal_points;
    for (int side = 0; side < 2; ++side)
      goal_points[side] = (*map)->Project(
          {(side ? 1 : -1) * ((*map)->layout.length / 2 - (*map)->layout.goal_inset), 0},
          rotation,
          (*session)->width(),
          (*session)->height());
    for (size_t i = 0; i < trial->frames.size(); ++i) {
      const auto& a = native->frames[i];
      const auto& b = trial->frames[i];
      if (!a.follower || !b.follower)
        continue;
      const auto info = b.rink_camera.value_or(hm::playtracker_replay::RinkCameraInfo{});
      const auto visible = [&](const hm::BBox& box) {
        if (!info.goal || !goal_points[info.goal > 0])
          return false;
        const auto point = *goal_points[info.goal > 0];
        const double x = point.x * (*session)->width(), y = point.y * (*session)->height();
        return x >= box.left && x <= box.right && y >= box.top && y <= box.bottom;
      };
      const bool a_goal = visible(*a.follower), b_goal = visible(*b.follower);
      out << a.sample_id << ',' << (a.pts_ns - trial->frames.front().pts_ns) / 1e9;
      for (const auto& box : {*a.follower, *b.follower})
        out << ',' << box.left << ',' << box.top << ',' << box.right << ',' << box.bottom;
      out << ',' << info.speed_mps << ',' << info.lead_pixels << ',' << info.zoom_ratio << ',' << info.goal << ','
          << info.located << ',' << info.active << ',' << a_goal << ',' << b_goal << '\n';
      ++compared;
      active += info.active;
      goals += info.goal != 0;
      baseline_goal_visible += a_goal;
      rink_goal_visible += b_goal;
      max_speed = std::max(max_speed, info.speed_mps);
      width_ratio += b.follower->width() / a.follower->width();
      if (i && native->frames[i - 1].follower && trial->frames[i - 1].follower) {
        const auto& previous_a = *native->frames[i - 1].follower;
        const auto& previous_b = *trial->frames[i - 1].follower;
        native_travel += std::abs(a.follower->left + a.follower->right - previous_a.left - previous_a.right) / 2;
        rink_travel += std::abs(b.follower->left + b.follower->right - previous_b.left - previous_b.right) / 2;
      }
    }
    if (!compared)
      throw std::runtime_error("No comparable camera frames in the selected range");
    // Exclusive creation also protects inputs and symlinks if another process
    // creates the output after the initial existence check.
    FILE* file = std::fopen(argv[5], "wx");
    if (!file)
      throw std::runtime_error("Could not create comparison CSV (output must not exist)");
    const auto contents = out.str();
    const bool written = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    const int closed = std::fclose(file);
    if (!written || closed != 0)
      throw std::runtime_error("Could not write comparison CSV");
    std::cout << "frames=" << trial->frames.size() << " active=" << active << " goal_frames=" << goals
              << " max_collective_speed_mps=" << max_speed << " mean_width_ratio=" << width_ratio / compared
              << " native_pan_pixels=" << native_travel << " rink_pan_pixels=" << rink_travel << '\n';
    std::cout << "goal_point_in_requested_frame: baseline=" << baseline_goal_visible << '/' << goals
              << " rink=" << rink_goal_visible << '/' << goals << '\n';
    std::cout << "metadata_replay_ms: native="
              << std::chrono::duration<double, std::milli>(guide_start - native_start).count()
              << " rink=" << std::chrono::duration<double, std::milli>(finished - guide_start).count()
              << " (includes guide initialization; excludes preparation and all video work)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
