#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "hstream/src/libs/stitching/GameConfig.h"

namespace {

namespace fs = std::filesystem;

constexpr auto kCalibrationTimeout = std::chrono::minutes(4);
constexpr auto kControlTimeout = std::chrono::seconds(20);

std::string user_home() {
  if (const char* home = std::getenv("HOME"); home && *home) {
    return home;
  }
  const passwd* entry = ::getpwuid(::getuid());
  return entry && entry->pw_dir ? entry->pw_dir : "";
}

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

bool write_all(int fd, const std::string& value) {
  size_t written = 0;
  while (written < value.size()) {
    const ssize_t result = ::write(fd, value.data() + written, value.size() - written);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    written += static_cast<size_t>(result);
  }
  return true;
}

bool run_command(
    const std::vector<std::string>& arguments,
    const std::vector<std::pair<std::string, std::string>>& env) {
  const pid_t child = ::fork();
  if (child == 0) {
    for (const auto& [name, value] : env) {
      ::setenv(name.c_str(), value.c_str(), 1);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    _exit(127);
  }
  int status = 0;
  return child > 0 && ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

class PipelineProcess {
 public:
  ~PipelineProcess() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
    }
    if (input_ >= 0) {
      ::close(input_);
    }
    if (output_ >= 0) {
      ::close(output_);
    }
  }

  bool Start(
      const fs::path& executable,
      const fs::path& pipeline_config,
      const fs::path& game_root,
      const fs::path& plugin_directory,
      const std::string& invalidation_id,
      int control_points,
      bool start_from_features,
      const std::string& stitch_frame_time = {},
      int time_limit_seconds = 0,
      const std::string& stitch_rotate_degrees = {},
      bool supply_runtime_invalidation = true,
      int rink_inference_delay_ms = 0,
      bool supply_control_points_environment = true,
      int same_stage_instances = 1,
      int completion_timeout_ms = 0,
      bool suppress_calibration_completion = false,
      int pipeline_recreate_seconds = 0) {
    int input_pipe[2];
    int output_pipe[2];
    if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) {
      return false;
    }
    pid_ = ::fork();
    if (pid_ == 0) {
      ::dup2(input_pipe[0], STDIN_FILENO);
      ::dup2(output_pipe[1], STDOUT_FILENO);
      ::dup2(output_pipe[1], STDERR_FILENO);
      ::close(input_pipe[0]);
      ::close(input_pipe[1]);
      ::close(output_pipe[0]);
      ::close(output_pipe[1]);
      ::setenv("HM_GAME_DIR", game_root.c_str(), 1);
      const std::string home = user_home();
      if (!home.empty()) {
        ::setenv("HOME", home.c_str(), 1);
      }
      if (supply_runtime_invalidation) {
        ::setenv("HSTREAM_CALIBRATION_PENDING", "1", 1);
        ::setenv("HSTREAM_CALIBRATION_INVALIDATION_ID", invalidation_id.c_str(), 1);
      } else {
        ::unsetenv("HSTREAM_CALIBRATION_PENDING");
        ::unsetenv("HSTREAM_CALIBRATION_INVALIDATION_ID");
      }
      if (supply_control_points_environment) {
        ::setenv("HM_MAX_CONTROL_POINTS", std::to_string(control_points).c_str(), 1);
      } else {
        ::unsetenv("HM_MAX_CONTROL_POINTS");
      }
      ::setenv("USE_NEW_NVSTREAMMUX", "yes", 1);
      ::setenv("GST_DEBUG", "NVDS_APP:4", 1);
      ::setenv("GST_PLUGIN_PATH", plugin_directory.c_str(), 1);
      if (rink_inference_delay_ms > 0) {
        ::setenv("HM_TEST_RINK_INFERENCE_DELAY_MS", std::to_string(rink_inference_delay_ms).c_str(), 1);
      } else {
        ::unsetenv("HM_TEST_RINK_INFERENCE_DELAY_MS");
      }
      if (same_stage_instances > 1) {
        // dGPU startup normally serializes PAUSED preroll. Exercise the same
        // concurrent calibration path used on integrated/ARM systems.
        ::setenv("HM_TEST_CONCURRENT_STITCHING_CALIBRATION", "1", 1);
      } else {
        ::unsetenv("HM_TEST_CONCURRENT_STITCHING_CALIBRATION");
      }
      if (completion_timeout_ms > 0) {
        ::setenv("HM_TEST_STITCH_FRAME_COMPLETION_TIMEOUT_MS", std::to_string(completion_timeout_ms).c_str(), 1);
      } else {
        ::unsetenv("HM_TEST_STITCH_FRAME_COMPLETION_TIMEOUT_MS");
      }
      if (suppress_calibration_completion) {
        ::setenv("HM_TEST_SUPPRESS_STITCHING_CALIBRATION_COMPLETION", "1", 1);
      } else {
        ::unsetenv("HM_TEST_SUPPRESS_STITCHING_CALIBRATION_COMPLETION");
      }
      if (start_from_features && supply_runtime_invalidation) {
        ::setenv("HSTREAM_CALIBRATION_START_STAGE", "features", 1);
      } else {
        ::unsetenv("HSTREAM_CALIBRATION_START_STAGE");
      }
      std::vector<std::string> arguments = {
          executable.string(),
          "-g",
          "proto",
      };
      for (int instance = 0; instance < same_stage_instances; ++instance) {
        arguments.push_back("-c");
        arguments.push_back(pipeline_config.string());
      }
      arguments.push_back("--enable-sources=URI-MULTIPLE");
      arguments.push_back("--enable-sinks=FAKE");
      if (supply_runtime_invalidation) {
        arguments.push_back("--clean-expected-invalidation-id=" + invalidation_id);
      }
      if (!stitch_frame_time.empty()) {
        arguments.push_back("--stitch-frame-time=" + stitch_frame_time);
      }
      if (time_limit_seconds > 0) {
        arguments.push_back("--time-limit=" + std::to_string(time_limit_seconds));
      }
      if (!stitch_rotate_degrees.empty()) {
        arguments.push_back("--stitch-rotate-degrees=" + stitch_rotate_degrees);
      }
      if (pipeline_recreate_seconds > 0) {
        arguments.push_back(
            "--options=pipeline.tests.pipeline-recreate-sec=" + std::to_string(pipeline_recreate_seconds));
      }
      std::vector<char*> argv;
      argv.reserve(arguments.size() + 1);
      for (std::string& argument : arguments) {
        argv.push_back(argument.data());
      }
      argv.push_back(nullptr);
      ::execv(executable.c_str(), argv.data());
      _exit(127);
    }
    ::close(input_pipe[0]);
    ::close(output_pipe[1]);
    input_ = input_pipe[1];
    output_ = output_pipe[0];
    const int flags = ::fcntl(output_, F_GETFL, 0);
    return pid_ > 0 && flags >= 0 && ::fcntl(output_, F_SETFL, flags | O_NONBLOCK) == 0;
  }

  bool Send(const std::string& command) const {
    return write_all(input_, command);
  }

  bool Interrupt() const {
    return pid_ > 0 && ::kill(pid_, SIGINT) == 0;
  }

  size_t Mark() {
    Drain();
    return output_text_.size();
  }

  bool WaitFor(
      const std::string& text,
      size_t after = 0,
      std::chrono::steady_clock::duration timeout = kControlTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      Drain();
      if (output_text_.find(text, after) != std::string::npos) {
        return true;
      }
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return false;
      }
      pollfd fd{output_, POLLIN, 0};
      ::poll(&fd, 1, 50);
    }
    Drain();
    return output_text_.find(text, after) != std::string::npos;
  }

  bool WaitForProgressAtOrBeyond(
      int minimum_video_seconds,
      size_t after,
      std::chrono::steady_clock::duration timeout = kControlTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      Drain();
      if (HasProgressAtOrBeyond(minimum_video_seconds, after)) {
        return true;
      }
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return false;
      }
      pollfd fd{output_, POLLIN, 0};
      ::poll(&fd, 1, 50);
    }
    Drain();
    return HasProgressAtOrBeyond(minimum_video_seconds, after);
  }

  bool WaitForExit(int* exit_code, std::chrono::steady_clock::duration timeout = kControlTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      Drain();
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        if (exit_code) {
          *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }

  const std::string& output() {
    Drain();
    return output_text_;
  }

  void DumpOutput(const char* label) {
    std::cerr << "--- " << label << " output ---\n" << output() << "\n--- end output ---\n";
  }

 private:
  bool HasProgressAtOrBeyond(int minimum_video_seconds, size_t after) const {
    size_t position = after;
    while ((position = output_text_.find("**PERF:", position)) != std::string::npos) {
      const size_t line_end = output_text_.find('\n', position);
      const size_t video_position = output_text_.find("Video ", position);
      if (video_position != std::string::npos && (line_end == std::string::npos || video_position < line_end)) {
        int hours = 0;
        int minutes = 0;
        int seconds = 0;
        if (std::sscanf(output_text_.c_str() + video_position, "Video %d:%d:%d", &hours, &minutes, &seconds) == 3 &&
            hours * 3600 + minutes * 60 + seconds >= minimum_video_seconds) {
          return true;
        }
      }
      position += 7;
    }
    return false;
  }

  void Drain() {
    char buffer[4096];
    while (output_ >= 0) {
      const ssize_t size = ::read(output_, buffer, sizeof(buffer));
      if (size > 0) {
        output_text_.append(buffer, static_cast<size_t>(size));
        continue;
      }
      if (size < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
  }

  pid_t pid_{-1};
  int input_{-1};
  int output_{-1};
  std::string output_text_;
};

bool write_pipeline_config(const fs::path& path) {
  std::ofstream output(path);
  output << R"YAML(application:
  stage: 0
  enable-perf-measurement: 1
  perf-measurement-interval-sec: 1
  complete-configuration: 1
tiled-display:
  enable: 0
source0:
  enable: 1
  type: 3
  num-sources: 1
  gpu-id: 0
  cuda-memory-type: 0
  source-id: 0
source1:
  enable: 1
  type: 3
  num-sources: 1
  gpu-id: 0
  cuda-memory-type: 0
  source-id: 1
sink0:
  enable: 1
  sink-id: 0
  type: 1
  sync: 1
  source-id: 0
  gpu-id: 0
streammux:
  width: 640
  height: 480
  batch-size: 2
  live-source: 0
  buffer-pool-size: 8
  num-surfaces-per-frame: 1
  batched-push-timeout: 2147483647
  sync-inputs: 0
hmstitcher:
  enable: 1
  gpu-id: 0
  nvbuf-memory-type: 0
  plugin-type: hmstitcher
  one-pass-mode: 1
  show: 0
  num-batch-buffers: 1
  num-output-buffers: 4
)YAML";
  return output.good();
}

bool write_game_config(
    const fs::path& path,
    int control_points,
    const std::string& invalidation_id,
    bool artifacts_invalidated) {
  std::ofstream output(path);
  output << "game:\n"
         << "  videos:\n"
         << "    left: [cam1/GX010001.MP4]\n"
         << "    right: [cam2/GX010002.MP4]\n"
         << "  stitching:\n"
         << "    frame_offsets:\n"
         << "      left: 0\n"
         << "      right: 0\n"
         << "hstream_ui:\n"
         << "  video_roles:\n"
         << "    left: [cam1/GX010001.MP4]\n"
         << "    right: [cam2/GX010002.MP4]\n"
         << "  stitching_calibration:\n"
         << "    control_points: " << control_points << "\n"
         << "    status: pending\n"
         << "    stale_from: features\n"
         << "    artifacts_invalidated: " << (artifacts_invalidated ? "true" : "false") << "\n"
         << "    invalidation_id: " << invalidation_id << "\n";
  return output.good();
}

bool write_saved_stitch_time_invalidation(
    const fs::path& path,
    const std::string& stitch_frame_time,
    const std::string& invalidation_id) {
  try {
    YAML::Node config = YAML::LoadFile(path.string());
    config["game"]["stitching"]["frame_offsets"]["left"] = 0;
    config["game"]["stitching"]["frame_offsets"]["right"] = 0;
    config["stitching"]["stitch_frame_time"] = stitch_frame_time;
    YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
    calibration["status"] = "pending";
    calibration["stale_from"] = "input";
    calibration["artifacts_invalidated"] = false;
    calibration["invalidation_id"] = invalidation_id;
    return hm::stitching::publish_game_config(path.parent_path(), YAML::Dump(config) + "\n").ok();
  } catch (const YAML::Exception&) {
    return false;
  }
}

bool write_rink_mask_invalidation(const fs::path& path, const std::string& invalidation_id) {
  try {
    YAML::Node config = YAML::LoadFile(path.string());
    YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
    calibration["status"] = "pending";
    calibration["stale_from"] = "rink-mask";
    calibration["artifacts_invalidated"] = true;
    calibration["invalidation_id"] = invalidation_id;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(path.parent_path(), error)) {
      if (error)
        return false;
      const std::string name = entry.path().filename().string();
      if (name.rfind("rink_mask_", 0) == 0 && entry.path().extension() == ".png") {
        fs::remove(entry.path(), error);
        if (error)
          return false;
      }
    }
    return hm::stitching::publish_game_config(path.parent_path(), YAML::Dump(config) + "\n").ok();
  } catch (const YAML::Exception&) {
    return false;
  }
}

bool remove_rink_masks(const fs::path& game_dir) {
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(game_dir, error)) {
    if (error)
      return false;
    const std::string name = entry.path().filename().string();
    if (name.rfind("rink_mask_", 0) == 0 && entry.path().extension() == ".png") {
      fs::remove(entry.path(), error);
      if (error)
        return false;
    }
  }
  return true;
}

bool clean_from_control_points(
    const fs::path& executable,
    const fs::path& pipeline_config,
    const fs::path& game_root,
    const std::string& invalidation_id) {
  return run_command(
      {executable.string(),
       "-g",
       "proto",
       "-c",
       pipeline_config.string(),
       "--clean-from-control-points",
       "--clean-expected-invalidation-id=" + invalidation_id},
      {{"HM_GAME_DIR", game_root.string()}, {"USE_NEW_NVSTREAMMUX", "yes"}});
}

bool stop_successfully(PipelineProcess* process, const char* message) {
  if (!expect(process->Interrupt(), message)) {
    return false;
  }
  int exit_code = -1;
  return expect(process->WaitForExit(&exit_code), "pipeline must stop promptly after SIGINT") &&
      expect(exit_code == 0, "pipeline must exit successfully after SIGINT");
}

bool runtime_caps_succeeded(PipelineProcess* process, const char* message) {
  const std::string& output = process->output();
  return expect(
      output.find("Failed to update runtime output caps downstream") == std::string::npos &&
          output.find("Failed to push runtime output caps downstream") == std::string::npos,
      message);
}

} // namespace

int main(int argc, char** argv) {
  ::signal(SIGPIPE, SIG_IGN);
  if (argc != 3) {
    std::cerr << "FAIL: expected hstream-cli and videoprep plugin paths\n";
    return 1;
  }
  bool ok = true;
  std::string pattern = (fs::temp_directory_path() / "pipeline-stitching-calibration-test-XXXXXX").string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    std::cerr << "FAIL: unable to create test directory\n";
    return 1;
  }
  const fs::path root(pattern);
  const fs::path game_root = root / "games";
  const fs::path game = game_root / "proto";
  const fs::path pipeline_config = root / "pipeline.yaml";
  const fs::path plugin_directory = fs::path(argv[2]).parent_path();
  fs::create_directories(game / "cam1");
  fs::create_directories(game / "cam2");
  ok &= expect(write_pipeline_config(pipeline_config), "pipeline config must be written");
  ok &= expect(
      run_command(
          {"ffmpeg",
           "-hide_banner",
           "-loglevel",
           "error",
           "-y",
           "-f",
           "lavfi",
           "-i",
           "testsrc2=size=960x540:rate=15,drawgrid=width=37:height=29:thickness=2:color=white@0.6,format=yuv420p",
           "-f",
           "lavfi",
           "-i",
           "sine=frequency=1000:sample_rate=48000",
           "-filter_complex",
           "[0:v]split=2[l][r];[l]crop=640:480:0:30[left];[r]crop=640:480:320:30[right];"
           "[1:a]asplit=2[left_audio][right_audio]",
           "-map",
           "[left]",
           "-map",
           "[left_audio]",
           "-t",
           "60",
           "-c:v",
           "libx264",
           "-preset",
           "ultrafast",
           "-g",
           "15",
           "-pix_fmt",
           "yuv420p",
           "-c:a",
           "aac",
           (game / "cam1" / "GX010001.MP4").string(),
           "-map",
           "[right]",
           "-map",
           "[right_audio]",
           "-t",
           "60",
           "-c:v",
           "libx264",
           "-preset",
           "ultrafast",
           "-g",
           "15",
           "-pix_fmt",
           "yuv420p",
           "-c:a",
           "aac",
           (game / "cam2" / "GX010002.MP4").string()},
          {}),
      "overlapping camera videos must be generated");

  PipelineProcess periodic_recreation;
  if (ok) {
    ok = [&] {
      if (!expect(
              write_game_config(game / "config.yaml", 128, "periodic-recreation", false),
              "periodic recreation calibration config must be written") ||
          !expect(
              periodic_recreation.Start(
                  argv[1],
                  pipeline_config,
                  game_root,
                  plugin_directory,
                  "periodic-recreation",
                  128,
                  false,
                  /*stitch_frame_time=*/"00:00:59.900",
                  /*time_limit_seconds=*/0,
                  /*stitch_rotate_degrees=*/{},
                  /*supply_runtime_invalidation=*/true,
                  /*rink_inference_delay_ms=*/0,
                  /*supply_control_points_environment=*/true,
                  /*same_stage_instances=*/1,
                  /*completion_timeout_ms=*/0,
                  /*suppress_calibration_completion=*/false,
                  /*pipeline_recreate_seconds=*/1),
              "periodic recreation calibration pipeline must start") ||
          !expect(
              periodic_recreation.WaitFor(
                  "Deferring periodic pipeline recreation until stitching calibration completes",
                  0,
                  kCalibrationTimeout),
              "periodic recreation must defer while one-pass calibration is active")) {
        return false;
      }
      return stop_successfully(
          &periodic_recreation, "periodic recreation deferral must leave calibration interruptible");
    }();
  }

  PipelineProcess initial;
  if (ok) {
    ok = [&] {
      if (!expect(
              write_game_config(game / "config.yaml", 128, "initial", false), "initial game config must be written") ||
          !expect(
              initial.Start(
                  argv[1],
                  pipeline_config,
                  game_root,
                  plugin_directory,
                  "initial",
                  128,
                  false,
                  /*stitch_frame_time=*/"00:00:59.900",
                  /*time_limit_seconds=*/1),
              "initial calibration pipeline must start") ||
          !expect(
              initial.WaitFor("HSTREAM_CALIBRATION stage=input status=complete", 0, kCalibrationTimeout),
              "initial calibration must capture synchronized inputs") ||
          !expect(
              initial.WaitFor("HSTREAM_CALIBRATION stage=orientation status=complete", 0, kCalibrationTimeout),
              "initial calibration must configure orientation") ||
          !expect(
              initial.WaitFor("Received EOS while awaiting calibration restart", 0, kCalibrationTimeout),
              "near-EOS calibration must preserve the bus watch until its restart is scheduled") ||
          !expect(
              initial.WaitFor("HSTREAM_CALIBRATION stage=calibration status=complete", 0, kCalibrationTimeout),
              "initial calibration must complete") ||
          !expect(
              initial.WaitFor("playback restarted after stitch-frame calibration", 0, kCalibrationTimeout),
              "a near-EOS stitch frame beyond the time limit must still restart normal playback") ||
          !expect(initial.WaitFor("Stitched canvas size:"), "initial calibration must publish the real canvas")) {
        return false;
      }
      const size_t restart_position = initial.output().rfind("playback restarted after stitch-frame calibration");
      if (!expect(
              initial.WaitFor("Pipeline running", restart_position),
              "normal playback must reach PLAYING after stitch-frame calibration")) {
        return false;
      }
      return stop_successfully(
          &initial, "post-calibration playback must remain controllable after bypassing the calibration time limit");
    }();
  }

  PipelineProcess missing_completion;
  if (ok) {
    ok = [&] {
      if (!expect(
              write_rink_mask_invalidation(game / "config.yaml", "missing-completion"),
              "missing-completion rink invalidation must be written") ||
          !expect(
              missing_completion.Start(
                  argv[1],
                  pipeline_config,
                  game_root,
                  plugin_directory,
                  "missing-completion",
                  128,
                  false,
                  /*stitch_frame_time=*/"00:00:59.900",
                  /*time_limit_seconds=*/0,
                  /*stitch_rotate_degrees=*/{},
                  /*supply_runtime_invalidation=*/true,
                  /*rink_inference_delay_ms=*/0,
                  /*supply_control_points_environment=*/true,
                  /*same_stage_instances=*/1,
                  /*completion_timeout_ms=*/15'000,
                  /*suppress_calibration_completion=*/true),
              "missing-completion calibration pipeline must start") ||
          !expect(
              missing_completion.WaitFor(
                  "suppressing calibration completion for lifecycle test", 0, kCalibrationTimeout),
              "missing-completion calibration must reach its completion boundary") ||
          !expect(
              missing_completion.WaitFor(
                  "Timed out waiting for stitching calibration completion after EOS", 0, std::chrono::seconds(30)),
              "EOS without a valid stitching completion must fail after a bounded wait")) {
        return false;
      }
      int exit_code = 0;
      return expect(
                 missing_completion.WaitForExit(&exit_code, std::chrono::seconds(10)),
                 "missing-completion calibration must exit after its completion timeout") &&
          expect(exit_code != 0, "missing-completion calibration must return a failure status");
    }();
  }

  PipelineProcess rotated;
  PipelineProcess saved_config;
  PipelineProcess completed_missing;
  PipelineProcess multi_context_nonzero;
  PipelineProcess multi_context_zero;
  std::string zero_time_invalidation_id;
  if (ok) {
    ok = [&] {
      if (!expect(
              write_saved_stitch_time_invalidation(game / "config.yaml", "00:00:00.500", "saved-config-time"),
              "saved stitch-time invalidation must be written over existing artifacts") ||
          !expect(
              saved_config.Start(
                  argv[1],
                  pipeline_config,
                  game_root,
                  plugin_directory,
                  "saved-config-time",
                  128,
                  false,
                  /*stitch_frame_time=*/{},
                  /*time_limit_seconds=*/0,
                  /*stitch_rotate_degrees=*/{},
                  /*supply_runtime_invalidation=*/false,
                  /*rink_inference_delay_ms=*/0,
                  /*supply_control_points_environment=*/false),
              "standalone launch must start from the saved invalidation") ||
          !expect(
              saved_config.WaitFor(
                  "Using persisted stitching calibration control-point limit 128", 0, kCalibrationTimeout),
              "standalone launch must restore the saved control-point limit without a pre-seeded environment") ||
          !expect(
              saved_config.WaitFor("HSTREAM_CALIBRATION stage=features status=started", 0, kCalibrationTimeout),
              "standalone launch must rebuild existing artifacts after a saved stitch-time change")) {
        return false;
      }
      const size_t restart_mark = saved_config.Mark();
      if (!expect(
              saved_config.WaitFor(
                  "playback restarted after stitch-frame calibration", restart_mark, kCalibrationTimeout),
              "standalone launch must honor the nonzero fractional stitch time from config.yaml") ||
          !expect(
              saved_config.WaitFor("Pipeline running", restart_mark),
              "standalone saved-time playback must reach PLAYING after calibration")) {
        return false;
      }
      return stop_successfully(&saved_config, "standalone saved-time playback must accept Stop/SIGINT");
    }();
  }

  if (ok) {
    ok = [&] {
      if (!expect(remove_rink_masks(game), "completed-config fixture must remove only the saved rink mask") ||
          !expect(
              completed_missing.Start(
                  argv[1],
                  pipeline_config,
                  game_root,
                  plugin_directory,
                  "saved-config-time",
                  128,
                  false,
                  /*stitch_frame_time=*/{},
                  /*time_limit_seconds=*/0,
                  /*stitch_rotate_degrees=*/{},
                  /*supply_runtime_invalidation=*/false,
                  /*rink_inference_delay_ms=*/0,
                  /*supply_control_points_environment=*/false),
              "completed config with a missing artifact must start standalone recalibration") ||
          !expect(
              completed_missing.WaitFor(
                  "Using persisted stitching calibration control-point limit 128", 0, kCalibrationTimeout),
              "runtime-discovered recalibration must restore completed config control points") ||
          !expect(
              completed_missing.WaitFor("playback restarted after stitch-frame calibration", 0, kCalibrationTimeout),
              "runtime-discovered recalibration must honor the saved nonzero stitch time") ||
          !expect(
              completed_missing.WaitFor(
                  "Pipeline running",
                  completed_missing.output().rfind("playback restarted after stitch-frame calibration")),
              "runtime-discovered recalibration must reach normal PLAYING state")) {
        return false;
      }
      return stop_successfully(
          &completed_missing, "runtime-discovered completed-config recalibration must accept Stop/SIGINT");
    }();
  }

  if (ok) {
    ok = [&] {
      if (!expect(remove_rink_masks(game), "multi-context nonzero fixture must remove the saved rink mask") ||
          !expect(
              multi_context_nonzero.Start(
                  argv[1],
                  pipeline_config,
                  game_root,
                  plugin_directory,
                  "saved-config-time",
                  128,
                  false,
                  /*stitch_frame_time=*/{},
                  /*time_limit_seconds=*/0,
                  /*stitch_rotate_degrees=*/{},
                  /*supply_runtime_invalidation=*/false,
                  /*rink_inference_delay_ms=*/2000,
                  /*supply_control_points_environment=*/false,
                  /*same_stage_instances=*/2),
              "two same-stage nonzero calibration contexts must start") ||
          !expect(
              multi_context_nonzero.WaitFor(
                  "HSTREAM_CALIBRATION stage=rink-mask status=started", 0, kCalibrationTimeout),
              "same-stage nonzero calibration must enter cancellable rink-mask work") ||
          !expect(
              multi_context_nonzero.WaitFor(
                  "playback restarted after stitch-frame calibration", 0, kCalibrationTimeout),
              "shared nonzero completion must recreate every calibration context") ||
          !expect(
              multi_context_nonzero.WaitFor(
                  "Pipeline running",
                  multi_context_nonzero.output().rfind("playback restarted after stitch-frame calibration")),
              "same-stage nonzero replacements must reach normal PLAYING state") ||
          !expect(
              multi_context_nonzero.output().find("Failed to pause pipeline before stitch-frame restart") ==
                  std::string::npos,
              "shared nonzero completion must not pause a busy calibration peer") ||
          !expect(
              multi_context_nonzero.output().find("ERROR from hmstitcher") == std::string::npos,
              "intentional peer calibration cancellation must not post a pipeline error")) {
        return false;
      }
      return stop_successfully(&multi_context_nonzero, "same-stage nonzero playback must remain controllable");
    }();
  }

  if (ok) {
    ok = [&] {
      if (!expect(remove_rink_masks(game), "multi-context zero fixture must remove the saved rink mask") ||
          !expect(
              multi_context_zero.Start(
                  argv[1],
                  pipeline_config,
                  game_root,
                  plugin_directory,
                  "saved-config-time",
                  128,
                  false,
                  /*stitch_frame_time=*/"00:00:00",
                  /*time_limit_seconds=*/0,
                  /*stitch_rotate_degrees=*/{},
                  /*supply_runtime_invalidation=*/false,
                  /*rink_inference_delay_ms=*/2000,
                  /*supply_control_points_environment=*/false,
                  /*same_stage_instances=*/2),
              "two same-stage zero-time calibration contexts must start") ||
          !expect(
              multi_context_zero.WaitFor("HSTREAM_CALIBRATION stage=rink-mask status=started", 0, kCalibrationTimeout),
              "same-stage zero-time calibration must enter cancellable rink-mask work") ||
          !expect(
              multi_context_zero.WaitFor("playback restarted after stitch-frame calibration", 0, kCalibrationTimeout),
              "shared zero-time completion must recreate peers to quiesce redundant workers") ||
          !expect(
              multi_context_zero.WaitFor(
                  "Pipeline running",
                  multi_context_zero.output().rfind("playback restarted after stitch-frame calibration")),
              "same-stage zero-time replacements must reach normal PLAYING state")) {
        return false;
      }
      auto persisted = hm::stitching::load_game_config_file(game / "config.yaml");
      if (persisted.ok() && persisted->has_value()) {
        zero_time_invalidation_id =
            (**persisted)["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>("");
      }
      if (!expect(
              persisted.ok() && persisted->has_value() && !(**persisted)["stitching"]["stitch_frame_time"] &&
                  !zero_time_invalidation_id.empty(),
              "an explicit zero CLI time must remove the nonzero dependency and own its new generation")) {
        return false;
      }
      return stop_successfully(&multi_context_zero, "same-stage zero-time playback must remain controllable");
    }();
  }

  if (ok) {
    ok = [&] {
      if (!expect(
              rotated.Start(
                  argv[1],
                  pipeline_config,
                  game_root,
                  plugin_directory,
                  zero_time_invalidation_id,
                  128,
                  false,
                  /*stitch_frame_time=*/"00:00:02",
                  /*time_limit_seconds=*/1,
                  /*stitch_rotate_degrees=*/"10"),
              "rotation-invalidated calibration pipeline must start") ||
          !expect(
              rotated.WaitFor("HSTREAM_CALIBRATION stage=calibration status=complete", 0, kCalibrationTimeout),
              "rotation-invalidated rink calibration must complete") ||
          !expect(
              rotated.WaitFor("playback restarted after stitch-frame calibration", 0, kCalibrationTimeout),
              "rotation invalidation must be reflected in the stitch-frame startup snapshot")) {
        return false;
      }
      const size_t restart_position = rotated.output().rfind("playback restarted after stitch-frame calibration");
      if (!expect(
              rotated.WaitFor("Pipeline running", restart_position),
              "rotation-invalidated normal playback must reach PLAYING after calibration")) {
        return false;
      }
      return stop_successfully(
          &rotated, "rotation-invalidated post-calibration playback must remain controllable after restart");
    }();
  }

  if (ok) {
    ok = expect(write_game_config(game / "config.yaml", 96, "cp-change", false), "CP-change config must be written") &&
        expect(
             clean_from_control_points(argv[1], pipeline_config, game_root, "cp-change"),
             "CP-change cleanup must invalidate only dependent artifacts") &&
        expect(fs::exists(game / "left.png") && fs::exists(game / "right.png"), "CP cleanup must preserve inputs") &&
        expect(!fs::exists(game / "hm_project.pto"), "CP cleanup must remove Hugin artifacts");
  }

  PipelineProcess resumed;
  size_t mark = 0;
  if (ok) {
    ok = [&] {
      if (!expect(
              resumed.Start(argv[1], pipeline_config, game_root, plugin_directory, "cp-change", 96, true),
              "CP-change calibration pipeline must start") ||
          !expect(
              resumed.WaitFor("HSTREAM_CALIBRATION stage=features status=started", 0, kCalibrationTimeout),
              "CP-change calibration must resume at features") ||
          !expect(
              resumed.WaitFor("HSTREAM_CALIBRATION stage=matching status=complete", 0, kCalibrationTimeout),
              "CP-change calibration must match control points") ||
          !expect(
              resumed.WaitFor("HSTREAM_CALIBRATION stage=optimizer status=complete", 0, kCalibrationTimeout),
              "CP-change calibration must optimize the panorama") ||
          !expect(
              resumed.WaitFor("HSTREAM_CALIBRATION stage=canvas status=complete", 0, kCalibrationTimeout),
              "CP-change calibration must generate the canvas") ||
          !expect(
              resumed.WaitFor("HSTREAM_CALIBRATION stage=calibration status=complete", 0, kCalibrationTimeout),
              "CP-change calibration must complete") ||
          !expect(
              resumed.output().find("HSTREAM_CALIBRATION stage=input status=") == std::string::npos &&
                  resumed.output().find("HSTREAM_CALIBRATION stage=orientation status=") == std::string::npos,
              "CP-change resume must not rediscover inputs or orientation") ||
          !runtime_caps_succeeded(&resumed, "CP-change resume must negotiate runtime output caps")) {
        return false;
      }
      mark = resumed.Mark();
      if (!expect(resumed.WaitForProgressAtOrBeyond(1, mark), "CP-change resume must push stitched video")) {
        return false;
      }
      mark = resumed.Mark();
      if (!expect(resumed.Send("p"), "pause command must be delivered to stitched pipeline") ||
          !expect(resumed.WaitFor("Pipeline paused", mark), "stitched pipeline must pause")) {
        return false;
      }
      mark = resumed.Mark();
      if (!expect(resumed.Send("r"), "resume command must be delivered to stitched pipeline") ||
          !expect(resumed.WaitFor("Pipeline running", mark), "stitched pipeline must resume")) {
        return false;
      }
      mark = resumed.Mark();
      if (!expect(
              resumed.WaitForProgressAtOrBeyond(1, mark), "stitched pipeline must keep advancing after pause/resume")) {
        return false;
      }
      if (!expect(
              resumed.output().find("Lossless camera frame barrier failed") == std::string::npos,
              "pause/resume must not disturb exact frame pairing")) {
        return false;
      }
      return stop_successfully(&resumed, "resumed calibrated pipeline must accept Stop/SIGINT");
    }();
  }

  if (ok) {
    ok =
        expect(
            write_game_config(game / "config.yaml", 80, "interrupted", false), "interruption config must be written") &&
        expect(
            clean_from_control_points(argv[1], pipeline_config, game_root, "interrupted"),
            "interruption cleanup must invalidate only dependent artifacts");
  }
  PipelineProcess interrupted;
  if (ok) {
    ok = expect(
             interrupted.Start(argv[1], pipeline_config, game_root, plugin_directory, "interrupted", 80, true),
             "interruptible calibration pipeline must start") &&
        expect(
             interrupted.WaitFor("HSTREAM_CALIBRATION stage=features status=started", 0, kCalibrationTimeout),
             "interruptible calibration must reach features") &&
        stop_successfully(&interrupted, "in-progress calibration must accept Stop/SIGINT") &&
        runtime_caps_succeeded(&interrupted, "interrupted calibration shutdown must not report a caps failure");
  }

  PipelineProcess recovered;
  if (ok) {
    ok = [&] {
      if (!expect(
              recovered.Start(argv[1], pipeline_config, game_root, plugin_directory, "interrupted", 80, true),
              "interrupted calibration must relaunch") ||
          !expect(
              recovered.WaitFor("HSTREAM_CALIBRATION stage=calibration status=complete", 0, kCalibrationTimeout),
              "interrupted calibration relaunch must complete") ||
          !runtime_caps_succeeded(&recovered, "interrupted calibration relaunch must negotiate runtime output caps")) {
        return false;
      }
      mark = recovered.Mark();
      return expect(
                 recovered.WaitForProgressAtOrBeyond(1, mark), "interrupted calibration relaunch must stitch video") &&
          stop_successfully(&recovered, "recovered calibrated pipeline must stop cleanly");
    }();
  }

  PipelineProcess interrupted_rink;
  if (ok) {
    ok = expect(
             write_rink_mask_invalidation(game / "config.yaml", "interrupted-rink"),
             "rink interruption config must preserve stitch maps while invalidating the rink mask") &&
        expect(
             interrupted_rink.Start(
                 argv[1],
                 pipeline_config,
                 game_root,
                 plugin_directory,
                 "interrupted-rink",
                 80,
                 false,
                 /*stitch_frame_time=*/{},
                 /*time_limit_seconds=*/0,
                 /*stitch_rotate_degrees=*/{},
                 /*supply_runtime_invalidation=*/true,
                 /*rink_inference_delay_ms=*/30000),
             "interruptible rink-mask pipeline must start") &&
        expect(
             interrupted_rink.WaitFor("HSTREAM_CALIBRATION stage=rink-mask status=started", 0, kCalibrationTimeout),
             "interruptible calibration must reach rink-mask inference") &&
        stop_successfully(&interrupted_rink, "in-progress rink-mask inference must accept Stop/SIGINT");
  }

  if (!ok) {
    initial.DumpOutput("initial calibration");
    missing_completion.DumpOutput("missing-completion calibration");
    saved_config.DumpOutput("standalone saved-time calibration");
    completed_missing.DumpOutput("completed config with missing artifact");
    multi_context_nonzero.DumpOutput("same-stage nonzero calibration");
    multi_context_zero.DumpOutput("same-stage zero calibration");
    rotated.DumpOutput("rotation-invalidated calibration");
    resumed.DumpOutput("CP-change resume");
    interrupted.DumpOutput("interrupted calibration");
    recovered.DumpOutput("recovered calibration");
    interrupted_rink.DumpOutput("interrupted rink-mask calibration");
    std::cerr << "fixture retained at " << root << '\n';
  } else {
    fs::remove_all(root);
  }
  return ok ? 0 : 1;
}
