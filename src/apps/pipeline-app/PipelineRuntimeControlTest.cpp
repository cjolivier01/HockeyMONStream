#include <fcntl.h>
#include <poll.h>
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

namespace {

namespace fs = std::filesystem;

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

bool run_command(const std::vector<std::string>& arguments) {
  const pid_t child = ::fork();
  if (child == 0) {
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

std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
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
      const fs::path& config,
      const std::string& source_type = "URI",
      const std::string& sink_type = "FAKE",
      bool headless_render_video = false,
      const std::vector<std::pair<std::string, std::string>>& environment = {},
      const std::vector<std::string>& extra_arguments = {},
      const fs::path& working_directory = {}) {
    const fs::path executable_path = fs::absolute(executable);
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
      ::setenv("HSTREAM_RUNTIME_ENV_READY", "1", 1);
      ::setenv("GST_DEBUG", "NVDS_APP:4", 1);
      ::setenv("USE_NEW_NVSTREAMMUX", "yes", 1);
      ::setenv("HSTREAM_RUNTIME_CONTROL_TEST_ROOT", config.parent_path().c_str(), 1);
      const std::string test_home = (config.parent_path() / "home").string();
      ::setenv("HOME", test_home.c_str(), 1);
      for (const auto& [name, value] : environment) {
        ::setenv(name.c_str(), value.c_str(), 1);
      }
      if (!working_directory.empty() && ::chdir(working_directory.c_str()) != 0) {
        _exit(126);
      }
      std::vector<std::string> arguments = {
          executable_path.string(),
          "-c",
          config.string(),
          "--enable-sources=" + source_type,
          "--enable-sinks=" + sink_type,
      };
      if (headless_render_video) {
        arguments.push_back("--headless-render-video");
      }
      arguments.insert(arguments.end(), extra_arguments.begin(), extra_arguments.end());
      std::vector<char*> argv;
      for (std::string& argument : arguments) {
        argv.push_back(argument.data());
      }
      argv.push_back(nullptr);
      ::execv(executable_path.c_str(), argv.data());
      _exit(127);
    }
    ::close(input_pipe[0]);
    ::close(output_pipe[1]);
    input_ = input_pipe[1];
    output_ = output_pipe[0];
    const int flags = ::fcntl(output_, F_GETFL, 0);
    ::fcntl(output_, F_SETFL, flags | O_NONBLOCK);
    return pid_ > 0;
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

  bool WaitFor(const std::string& text, size_t after = 0, std::chrono::seconds timeout = std::chrono::seconds(8)) {
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
      size_t after = 0,
      std::chrono::seconds timeout = std::chrono::seconds(8)) {
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

  bool WaitForExit(
      int* exit_code,
      std::chrono::seconds timeout = std::chrono::seconds(8),
      bool* exited_normally = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      Drain();
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        if (exited_normally) {
          *exited_normally = WIFEXITED(status);
        }
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

  void DumpOutput() {
    std::cerr << "--- hstream-cli output ---\n" << output() << "\n--- end output ---\n";
  }

  bool running() {
    int status = 0;
    if (pid_ > 0 && ::waitpid(pid_, &status, WNOHANG) == pid_) {
      pid_ = -1;
    }
    return pid_ > 0;
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
      position += std::string("**PERF:").size();
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

bool verify_telemetry_seek_rejection(
    const fs::path& executable,
    const fs::path& config,
    const fs::path& telemetry_csv_dir,
    const char* case_name,
    const fs::path& working_directory = {}) {
  auto check = [case_name](bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << case_name << ": " << message << '\n';
    }
    return condition;
  };

  PipelineProcess process;
  if (!check(
          process.Start(
              executable,
              config,
              "URI-MULTIPLE",
              "RENDER",
              true,
              {{"HM_TEST_PIPELINE_RECREATE_FAIL_AFTER_DESTROY", "1"}},
              {},
              working_directory),
          "telemetry capture seek process must start") ||
      !check(process.WaitFor("Pipeline running"), "telemetry capture pipeline must reach PLAYING") ||
      !check(
          process.WaitForProgressAtOrBeyond(1, 0, std::chrono::seconds(12)),
          "telemetry capture pipeline must process media before the seek command")) {
    process.DumpOutput();
    return false;
  }

  const size_t seek_mark = process.Mark();
  if (!check(process.Send("@seek 10000000000 11\n"), "telemetry capture seek command must be delivered") ||
      !check(
          process.WaitFor("HSTREAM_SEEK status=rejected generation=11 reason=telemetry-capture-active", seek_mark),
          "backend must reject reconstruction before destroying an active telemetry exporter") ||
      !check(
          process.output().find("HSTREAM_PIPELINE_RECREATE status=injected-failure", seek_mark) == std::string::npos,
          "telemetry seek rejection must occur before reconstruction can tear down the exporter") ||
      !check(
          process.WaitForProgressAtOrBeyond(2, seek_mark, std::chrono::seconds(12)),
          "the original telemetry pipeline must continue after the rejected seek") ||
      !check(process.Send("q"), "telemetry capture process quit command must be delivered")) {
    process.DumpOutput();
    return false;
  }

  int exit_code = -1;
  if (!check(
          process.WaitForExit(&exit_code, std::chrono::seconds(12)), "telemetry capture process must stop promptly") ||
      !check(exit_code == 0, "telemetry capture process must exit successfully")) {
    process.DumpOutput();
    return false;
  }

  const std::string telemetry_manifest = read_file(telemetry_csv_dir / "hstream_telemetry.json");
  return check(
      !telemetry_manifest.empty() && telemetry_manifest.find("\"writer_drained\": true") != std::string::npos &&
          fs::exists(telemetry_csv_dir / "detections.csv") &&
          !fs::exists(telemetry_csv_dir / "hstream_telemetry-1.json"),
      "a rejected seek must finalize one uninterrupted telemetry session with detections, never only a replacement "
      "segment");
}

bool verify_telemetry_seek_allowed_when_disabled(
    const fs::path& executable,
    const fs::path& config,
    const fs::path& telemetry_csv_dir) {
  PipelineProcess process;
  if (!expect(
          process.Start(
              executable,
              config,
              "URI-MULTIPLE",
              "RENDER",
              true,
              {{"HM_TEST_PIPELINE_RECREATE_FAIL_AFTER_DESTROY", "1"}}),
          "last-empty telemetry capture seek process must start") ||
      !expect(process.WaitFor("Pipeline running"), "last-empty telemetry capture pipeline must reach PLAYING") ||
      !expect(
          process.WaitForProgressAtOrBeyond(1, 0, std::chrono::seconds(12)),
          "last-empty telemetry capture pipeline must process media before the seek command")) {
    process.DumpOutput();
    return false;
  }

  const size_t seek_mark = process.Mark();
  if (!expect(process.Send("@seek 10000000000 12\n"), "last-empty telemetry seek command must be delivered") ||
      !expect(
          process.WaitFor(
              "HSTREAM_PIPELINE_RECREATE status=injected-failure phase=after-destroy",
              seek_mark,
              std::chrono::seconds(20)),
          "a final empty telemetry property must allow reconstruction to start") ||
      !expect(
          process.output().find(
              "HSTREAM_SEEK status=rejected generation=12 reason=telemetry-capture-active", seek_mark) ==
              std::string::npos,
          "an earlier nonempty telemetry alias must not override the final empty value") ||
      !expect(
          !fs::exists(telemetry_csv_dir / "hstream_telemetry.json"),
          "a final empty telemetry property must leave the exporter disabled") ||
      !expect(process.Interrupt(), "last-empty telemetry capture process SIGINT must be delivered")) {
    process.DumpOutput();
    return false;
  }

  int exit_code = -1;
  if (!expect(
          process.WaitForExit(&exit_code, std::chrono::seconds(12)),
          "last-empty telemetry capture process must stop promptly")) {
    process.DumpOutput();
    return false;
  }
  return true;
}

bool write_config(
    const fs::path& config,
    const fs::path& video,
    const fs::path& archive,
    bool recreate_pipeline = false,
    bool native_tracker = false) {
  std::ofstream output(config);
  output << "application:\n"
         << "  stage: 0\n"
         << "  enable-perf-measurement: 1\n"
         << "  perf-measurement-interval-sec: 1\n"
         << "tiled-display:\n"
         << "  enable: 0\n"
         << "source0:\n"
         << "  enable: 1\n"
         << "  type: 2\n"
         << "  uri: file://" << video.string() << "\n"
         << "  source-id: 0\n"
         << "  gpu-id: 0\n"
         << "  cuda-memory-type: 0\n"
         << "sink1:\n"
         << "  enable: 0\n"
         << "  sink-id: 1\n"
         << "  type: 3\n"
         << "  container: 2\n"
         << "  output-file: " << archive.string() << "\n"
         << "  codec: 1\n"
         << "  enc-type: 0\n"
         << "  sync: 1\n"
         << "  bitrate: 1000000\n"
         << "  profile: 0\n"
         << "  source-id: 0\n"
         << "  gpu-id: 0\n"
         << "sink0:\n"
         << "  enable: 1\n"
         << "  sink-id: 0\n"
         << "  type: 1\n"
         << "  sync: 1\n"
         << "  source-id: 0\n"
         << "  gpu-id: 0\n"
         << "sink2:\n"
         << "  enable: 0\n"
         << "  sink-id: 2\n"
         << "  type: 2\n"
         << "  sync: 1\n"
         << "  source-id: 0\n"
         << "  gpu-id: 0\n"
         << "streammux:\n"
         << "  width: 256\n"
         << "  height: 144\n"
         << "  batch-size: 1\n"
         << "  live-source: 0\n"
         << "  batched-push-timeout: 40000\n"
         << "  buffer-pool-size: 8\n"
         << "  num-surfaces-per-frame: 1\n"
         << "  gpu-id: 0\n";
  if (native_tracker) {
    output << "tracker:\n"
           << "  enable: 1\n"
           << "  tracker-width: 256\n"
           << "  tracker-height: 128\n"
           << "  ll-lib-file: /opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so\n"
           << "  ll-config-file: "
              "/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_tracker_NvDCF_perf.yml\n"
           << "  gpu-id: 0\n"
           << "  display-tracking-id: 1\n";
  }
  if (recreate_pipeline) {
    output << "tests:\n"
           << "  pipeline-recreate-sec: 5\n";
  }
  return output.good();
}

bool write_playlist_error_config(
    const fs::path& config,
    const fs::path& video,
    const fs::path& missing_second_chapter) {
  std::ofstream output(config);
  output << "application:\n"
         << "  stage: 0\n"
         << "tiled-display:\n"
         << "  enable: 0\n"
         << "source0:\n"
         << "  enable: 1\n"
         << "  type: 3\n"
         << "  uri: file://" << video.string() << "\n"
         << "  uri-list:\n"
         << "    - file://" << video.string() << "\n"
         << "  source-id: 0\n"
         << "  gpu-id: 0\n"
         << "  cuda-memory-type: 0\n"
         << "source1:\n"
         << "  enable: 1\n"
         << "  type: 3\n"
         << "  uri: file://" << missing_second_chapter.string() << "\n"
         << "  uri-list:\n"
         << "    - file://" << missing_second_chapter.string() << "\n"
         << "  source-id: 1\n"
         << "  gpu-id: 0\n"
         << "  cuda-memory-type: 0\n"
         << "sink0:\n"
         << "  enable: 1\n"
         << "  sink-id: 0\n"
         << "  type: 1\n"
         << "  sync: 0\n"
         << "  source-id: 0\n"
         << "  gpu-id: 0\n"
         << "streammux:\n"
         << "  width: 256\n"
         << "  height: 144\n"
         << "  batch-size: 2\n"
         << "  live-source: 0\n"
         << "  batched-push-timeout: 40000\n"
         << "  buffer-pool-size: 8\n"
         << "  num-surfaces-per-frame: 1\n"
         << "  gpu-id: 0\n";
  return output.good();
}

bool write_playtracker_config(const fs::path& config) {
  std::ofstream output(config);
  output << R"yaml(play-tracker:
  camera-name: GoPro
  no-wide-start: true
  ignore-largest-bbox: true
  fps-speed-scale: 1.0
  min-considered-group-velocity: 3.0
  group-ratio-threshold: 0.5
  group-velocity-speed-ratio: 0.3
  scale-speed-constraints: 3.0
  nonstop-delay-count: 2
  overshoot-scale-speed-ratio: 0.7
  overshoot-stop-delay-count: 6
  max-speed-ratio-x: 1.0
  max-speed-ratio-y: 1.0
  max-accel-ratio-x: 1.0
  max-accel-ratio-y: 1.0
  follower-box-min-height-ratio: 0.2
  zoom-in-aggressiveness: 25
  live-boxes:
    - name: current_roi
      time-to-dest-speed-limit-frames: 20
      time-to-dest-stop-speed-threshold: 0.25
      resizing-stop-on-dir-change-delay: 4
      resizing-cancel-stop-on-opposite-dir: true
      resizing-stop-cancel-hysteresis-frames: 10
      resizing-stop-delay-cooldown-frames: 2
      resizing-time-to-dest-speed-limit-frames: 10
      resizing-time-to-dest-stop-speed-threshold: 0.25
    - name: current_roi_aspect
      stop-translation-on-dir-change-delay: 10
      cancel-stop-on-opposite-dir: true
      cancel-stop-hysteresis-frames: 2
      stop-delay-cooldown-frames: 2
      post-nonstop-stop-delay-count: 6
      time-to-dest-speed-limit-frames: 20
      time-to-dest-stop-speed-threshold: 0.25
      resizing-stop-on-dir-change-delay: 4
      resizing-cancel-stop-on-opposite-dir: true
      resizing-stop-cancel-hysteresis-frames: 10
      resizing-stop-delay-cooldown-frames: 2
      resizing-time-to-dest-speed-limit-frames: 10
      resizing-time-to-dest-stop-speed-threshold: 0.25
      sticky-size-ratio-to-frame-width: 10.0
      sticky-translation-gaussian-mult: 5.0
      unsticky-translation-size-ratio: 0.75
      scale-dest-width: 1.45
      scale-dest-height: 1.45
)yaml";
  return output.good();
}

bool write_playtracker_runtime_config(const fs::path& config) {
  std::ofstream output(config);
  output << R"yaml(play-tracker:
  hstream-apply-to-fast-box: false
  hstream-apply-to-follower-box: false
  hstream-runtime-tuning:
    zoom-in-aggressiveness: 75
  live-boxes:
    - name: current_roi
    - name: current_roi_aspect
)yaml";
  return output.good();
}

bool write_playlist_seek_config(
    const fs::path& config,
    const fs::path& video,
    const fs::path& playtracker_config,
    int second_source_chapter_count = 2,
    const std::vector<std::pair<std::string, std::string>>& telemetry_properties = {}) {
  std::ofstream output(config);
  output << "application:\n"
         << "  stage: 0\n"
         << "  enable-perf-measurement: 1\n"
         << "  perf-measurement-interval-sec: 1\n"
         << "tiled-display:\n"
         << "  enable: 0\n";
  for (int source_id = 0; source_id < 2; ++source_id) {
    output << "source" << source_id << ":\n"
           << "  enable: 1\n"
           << "  type: 3\n"
           << "  uri: file://" << video.string() << "\n"
           << "  uri-list:\n";
    const int chapter_count = source_id == 1 ? second_source_chapter_count : 2;
    for (int chapter = 0; chapter < chapter_count; ++chapter) {
      output << "    - file://" << video.string() << "\n";
    }
    output << "  source-id: " << source_id << "\n"
           << "  gpu-id: 0\n"
           << "  cuda-memory-type: 0\n";
  }
  output << "sink0:\n"
         << "  enable: 1\n"
         << "  sink-id: 0\n"
         << "  type: 1\n"
         << "  sync: 1\n"
         << "  source-id: 0\n"
         << "  gpu-id: 0\n"
         << "sink2:\n"
         << "  enable: 0\n"
         << "  sink-id: 2\n"
         << "  type: 2\n"
         << "  sync: 1\n"
         << "  source-id: 0\n"
         << "  gpu-id: 0\n"
         << "streammux:\n"
         << "  width: 256\n"
         << "  height: 144\n"
         << "  batch-size: 2\n"
         << "  live-source: 0\n"
         << "  batched-push-timeout: 40000\n"
         << "  buffer-pool-size: 8\n"
         << "  num-surfaces-per-frame: 1\n"
         << "  gpu-id: 0\n"
         << "tracker:\n"
         << "  enable: 1\n"
         << "  tracker-width: 256\n"
         << "  tracker-height: 128\n"
         << "  ll-lib-file: /opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so\n"
         << "  ll-config-file: "
            "/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_tracker_NvDCF_perf.yml\n"
         << "  gpu-id: 0\n"
         << "  display-tracking-id: 1\n";
  output << "ds-playtracker:\n"
         << "  enable: 1\n"
         << "  gpu-id: 0\n"
         << "  unique-id: 75\n"
         << "  show: 0\n"
         << "  plugin-type: vpplaytracker\n"
         << "  config-file: " << playtracker_config.string() << "\n";
  if (!telemetry_properties.empty()) {
    output << "  private-properties:\n";
    for (const auto& [name, value] : telemetry_properties) {
      output << "    " << name << ": " << value << "\n";
    }
  }
  return output.good();
}

} // namespace

int main(int argc, char** argv) {
  ::signal(SIGPIPE, SIG_IGN);
  if (argc != 2) {
    std::cerr << "FAIL: expected hstream-cli path\n";
    return 1;
  }
  bool ok = true;
  std::string pattern = (fs::temp_directory_path() / "pipeline-runtime-control-test-XXXXXX").string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    std::cerr << "FAIL: unable to create test directory\n";
    return 1;
  }
  const fs::path root(pattern);
  const fs::path home = root / "home";
  const fs::path video = root / "input.mp4";
  const fs::path multi_track_video = root / "input-multi-track.mp4";
  const fs::path config = root / "pipeline.yaml";
  const fs::path tracker_config = root / "pipeline-tracker.yaml";
  const fs::path playlist_seek_config = root / "pipeline-playlist-seek.yaml";
  const fs::path multi_track_playlist_seek_config = root / "pipeline-playlist-multi-track-seek.yaml";
  const fs::path unequal_playlist_seek_config = root / "pipeline-playlist-unequal-seek.yaml";
  const fs::path telemetry_seek_config = root / "pipeline-playlist-telemetry-seek.yaml";
  const fs::path telemetry_csv_dir = root / "telemetry-seek";
  const fs::path telemetry_alias_seek_config = root / "pipeline-playlist-telemetry-alias-seek.yaml";
  const fs::path telemetry_alias_csv_dir = root / "telemetry-alias-seek";
  const fs::path telemetry_whitespace_seek_config = root / "pipeline-playlist-telemetry-whitespace-seek.yaml";
  const fs::path telemetry_whitespace_csv_dir = root / "   ";
  const fs::path telemetry_disabled_seek_config = root / "pipeline-playlist-telemetry-disabled-seek.yaml";
  const fs::path telemetry_disabled_csv_dir = root / "telemetry-disabled-seek";
  const fs::path playtracker_config = root / "playtracker.yaml";
  const fs::path playtracker_runtime_config = root / "playtracker-runtime.yaml";
  const fs::path recreate_config = root / "pipeline-recreate.yaml";
  const fs::path error_config = root / "pipeline-error.yaml";
  const fs::path archive = root / "archive.mkv";
  ok &= expect(fs::create_directory(home), "isolated pipeline-app HOME must be created");
  ok &= expect(
      run_command({
          "ffmpeg",    "-hide_banner", "-loglevel",
          "error",     "-y",           "-f",
          "lavfi",     "-i",           "testsrc2=size=256x144:rate=15,format=yuv420p",
          "-t",        "1800",         "-an",
          "-c:v",      "libx264",      "-preset",
          "ultrafast", "-g",           "15",
          "-pix_fmt",  "yuv420p",      video.string(),
      }),
      "synthetic seekable input must be generated");
  ok &= expect(
      run_command({
          "ffmpeg",
          "-hide_banner",
          "-loglevel",
          "error",
          "-y",
          "-i",
          video.string(),
          "-map",
          "0:v:0",
          "-map",
          "0:v:0",
          "-c:v",
          "copy",
          multi_track_video.string(),
      }),
      "synthetic multi-video-track input must be generated");
  ok &= expect(write_config(config, video, archive), "pipeline-app test config must be written");
  ok &= expect(
      write_config(tracker_config, video, archive, false, true),
      "pipeline-app native-tracker seek config must be written");
  ok &= expect(write_playtracker_config(playtracker_config), "playtracker base config must be written");
  ok &= expect(
      write_playtracker_runtime_config(playtracker_runtime_config), "playtracker live tuning config must be written");
  ok &= expect(
      write_playlist_seek_config(playlist_seek_config, video, playtracker_config),
      "pipeline-app multi-chapter native-tracker seek config must be written");
  ok &= expect(
      write_playlist_seek_config(multi_track_playlist_seek_config, multi_track_video, playtracker_config),
      "pipeline-app multi-track seek config must be written");
  ok &= expect(
      write_playlist_seek_config(unequal_playlist_seek_config, video, playtracker_config, 1),
      "pipeline-app unequal exact-pair seek config must be written");
  ok &= expect(fs::create_directory(telemetry_csv_dir), "telemetry seek directory must be created");
  ok &= expect(
      write_playlist_seek_config(
          telemetry_seek_config, video, playtracker_config, 2, {{"telemetry-csv-dir", telemetry_csv_dir.string()}}),
      "pipeline-app telemetry seek config must be written");
  ok &= expect(fs::create_directory(telemetry_alias_csv_dir), "telemetry alias seek directory must be created");
  ok &= expect(
      write_playlist_seek_config(
          telemetry_alias_seek_config,
          video,
          playtracker_config,
          2,
          {{"telemetry_csv_dir", telemetry_alias_csv_dir.string()}}),
      "pipeline-app telemetry alias seek config must be written");
  ok &=
      expect(fs::create_directory(telemetry_whitespace_csv_dir), "telemetry whitespace seek directory must be created");
  ok &= expect(
      write_playlist_seek_config(
          telemetry_whitespace_seek_config, video, playtracker_config, 2, {{"telemetry-csv-dir", R"yaml("   ")yaml"}}),
      "pipeline-app telemetry whitespace seek config must be written");
  ok &= expect(fs::create_directory(telemetry_disabled_csv_dir), "telemetry last-empty seek directory must be created");
  ok &= expect(
      write_playlist_seek_config(
          telemetry_disabled_seek_config,
          video,
          playtracker_config,
          2,
          {
              {"telemetry_csv_dir", telemetry_disabled_csv_dir.string()},
              {"telemetry-csv-dir", R"yaml("")yaml"},
          }),
      "pipeline-app telemetry last-empty seek config must be written");
  ok &=
      expect(write_config(recreate_config, video, archive, true), "pipeline-app recreate test config must be written");
  ok &= expect(
      write_playlist_error_config(error_config, video, root / "missing-second-chapter.mp4"),
      "pipeline-app error test config must be written");

  PipelineProcess process;
  ok &= expect(process.Start(argv[1], config), "hstream-cli process must start");
  ok &= expect(process.WaitFor("Pipeline running"), "pipeline-app must reach PLAYING");
  ok &= expect(
      process.WaitFor(
          "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"session\",\"requestId\":0,\"status\":\"ok\","
          "\"stage\":0,\"generation\":1}"),
      "pipeline-app must announce the active inspector stage/generation binding");
  ok &= expect(process.running(), "pipeline-app must keep processing after reaching PLAYING");
  const size_t graph_mark = process.Mark();
  ok &= expect(process.Send("@inspect-pipeline 101 0 1\n"), "pipeline graph inspection command must be delivered");
  ok &= expect(
      process.WaitFor(
          "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"graph\",\"requestId\":101,\"status\":\"ok\"",
          graph_mark),
      "backend must return a versioned structured pipeline graph");
  const std::string graph_output = process.output().substr(graph_mark);
  ok &= expect(
      graph_output.find("\"stage\":0,\"generation\":1") != std::string::npos &&
          graph_output.find("\"nodes\":[") != std::string::npos &&
          graph_output.find("\"edges\":[") != std::string::npos && graph_output.find("\"caps\":") == std::string::npos,
      "pipeline graph response must include nodes/pad connections without negotiated caps");
  constexpr const char* kQueuePath = "ODpwaXBlbGluZS8xMzptdWx0aV9zcmNfYmluLzEyOnNyY19zdWJfYmluMC81OnF1ZXVl";
  constexpr const char* kSilentProperty = "c2lsZW50";
  const size_t properties_mark = process.Mark();
  ok &= expect(
      process.Send(std::string("@inspect-properties 102 0 1 0 ") + kQueuePath + "\n"),
      "pipeline element property inspection command must be delivered");
  ok &= expect(
      process.WaitFor(
          "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"properties\",\"requestId\":102,"
          "\"status\":\"ok\"",
          properties_mark),
      "backend must return selected-node property names, types, values, and mutability");
  const std::string properties_output = process.output().substr(properties_mark);
  ok &= expect(
      properties_output.find("\"name\":\"silent\"") != std::string::npos &&
          properties_output.find("\"editable\":true") != std::string::npos,
      "a queue property explicitly mutable while PLAYING must be advertised as safely editable");
  const size_t stale_property_mark = process.Mark();
  ok &= expect(
      process.Send(
          std::string("@inspect-set-property 103 -1 1 0 ") + kQueuePath + " " + kSilentProperty + " dHJ1ZQ==\n"),
      "stale-stage inspector property mutation must be delivered");
  ok &= expect(
      process.WaitFor(
          "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"set-result\",\"requestId\":103,"
          "\"status\":\"error\",\"stage\":-1,\"generation\":1,"
          "\"message\":\"Stale pipeline inspector stage/generation",
          stale_property_mark),
      "backend must reject a mutation bound to a previous stage before touching the element");
  const size_t property_set_mark = process.Mark();
  ok &= expect(
      process.Send(
          std::string("@inspect-set-property 104 0 1 0 ") + kQueuePath + " " + kSilentProperty + " dHJ1ZQ==\n"),
      "approved inspector property mutation must be delivered");
  ok &= expect(
      process.WaitFor(
          "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"set-result\",\"requestId\":104,"
          "\"status\":\"ok\"",
          property_set_mark),
      "backend must accept an explicitly live-mutable scalar property");
  const size_t property_restore_mark = process.Mark();
  ok &= expect(
      process.Send(
          std::string("@inspect-set-property 105 0 1 0 ") + kQueuePath + " " + kSilentProperty + " ZmFsc2U=\n"),
      "inspector property restoration must be delivered");
  ok &= expect(
      process.WaitFor(
          "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"set-result\",\"requestId\":105,"
          "\"status\":\"ok\"",
          property_restore_mark),
      "backend must restore the live property through the guarded setter");
  const size_t unsafe_property_mark = process.Mark();
  ok &= expect(
      process.Send(std::string("@inspect-set-property 106 0 1 0 ") + kQueuePath + " bmFtZQ== aGFja2Vk\n"),
      "unsafe inspector property request must be delivered");
  ok &= expect(
      process.WaitFor(
          "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"set-result\",\"requestId\":106,"
          "\"status\":\"error\"",
          unsafe_property_mark),
      "backend must reject writable properties that are not explicitly safe while PLAYING");
  const size_t malformed_inspector_mark = process.Mark();
  ok &= expect(
      process.Send("@inspect-properties 107 0 1 0 not-base64\n"),
      "malformed pipeline inspector command must be delivered");
  ok &= expect(
      process.WaitFor(
          "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"command\",\"requestId\":0,"
          "\"status\":\"error\"",
          malformed_inspector_mark),
      "backend must reject malformed inspector tokens without executing a lookup");
  ok &= expect(
      process.WaitForProgressAtOrBeyond(1, 0, std::chrono::seconds(12)),
      "pipeline-app must advance its video position before controls");
  size_t seek_rejection_mark = process.Mark();
  ok &= expect(process.Send("@seek 10000000000 1\n"), "nonlocal seek command must be delivered");
  ok &= expect(
      process.WaitFor("HSTREAM_SEEK status=rejected generation=1 reason=nonlocal-output-active", seek_rejection_mark),
      "pipeline-app must reject seeking when the active sink is not local rendering");

  for (int iteration = 0; iteration < 3 && ok; ++iteration) {
    size_t mark = process.Mark();
    ok &= expect(process.Send("p"), "pause command must be delivered");
    ok &= expect(process.WaitFor("Pipeline paused", mark), "pipeline-app must reach PAUSED");
    mark = process.Mark();
    ok &= expect(process.Send("r"), "resume command must be delivered");
    ok &= expect(process.WaitFor("Pipeline running", mark), "pipeline-app must resume PLAYING");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }
  ok &= expect(process.running(), "pipeline-app must keep processing after repeated pause/resume");
  ok &= expect(process.Interrupt(), "SIGINT stop must be delivered");
  int exit_code = -1;
  ok &= expect(process.WaitForExit(&exit_code), "pipeline-app must stop promptly after SIGINT");
  ok &= expect(exit_code == 0, "pipeline-app must exit successfully after a user stop");
  ok &= expect(
      process.output().find("App run successful") != std::string::npos,
      "pipeline-app must report successful completion after a user stop");

  PipelineProcess relaunched_process;
  if (ok) {
    ok &= expect(
        relaunched_process.Start(argv[1], config, "URI", "RENDER", true),
        "pipeline-app must relaunch with a headless local-render sink after a clean stop");
    ok &= expect(relaunched_process.WaitFor("Pipeline running"), "relaunched pipeline-app must reach PLAYING");
    size_t pause_mark = relaunched_process.Mark();
    ok &= expect(relaunched_process.Send("p"), "local-render pipeline pause command must be delivered");
    ok &= expect(relaunched_process.WaitFor("Pipeline paused", pause_mark), "local-render pipeline must reach PAUSED");
    const size_t paused_seek_mark = relaunched_process.Mark();
    ok &= expect(relaunched_process.Send("@seek 10000000000 1\n"), "paused seek command must be delivered");
    ok &= expect(
        relaunched_process.WaitFor(
            "HSTREAM_SEEK status=rejected generation=1 reason=pipeline-not-playing", paused_seek_mark),
        "backend must reject a seek while local playback is paused");
    const size_t resume_mark = relaunched_process.Mark();
    ok &= expect(relaunched_process.Send("r"), "local-render pipeline resume command must be delivered");
    ok &= expect(relaunched_process.WaitFor("Pipeline running", resume_mark), "local-render pipeline must resume");
    ok &= expect(relaunched_process.Interrupt(), "local-render pipeline SIGINT stop must be delivered");
    exit_code = -1;
    ok &= expect(relaunched_process.WaitForExit(&exit_code), "relaunched pipeline-app must stop promptly after SIGINT");
    ok &= expect(exit_code == 0, "relaunched pipeline-app must exit successfully");
    ok &= expect(
        relaunched_process.output().find("App run successful") != std::string::npos,
        "relaunched pipeline-app must report successful completion");
  }

  PipelineProcess seek_process;
  if (ok) {
    ok &= expect(
        seek_process.Start(
            argv[1],
            playlist_seek_config,
            "URI-MULTIPLE",
            "RENDER",
            true,
            {
                {"HM_TEST_RUNTIME_PROPERTY_APPLY_DELAY_MS", "400"},
                {"HM_TEST_RUNTIME_SEEK_INJECT_PENDING_PLAYLIST_CALLBACK", "1"},
                {"HM_TEST_RUNTIME_SEEK_INJECT_REPLACEMENT_PLAYLIST_CALLBACK", "1"},
                {"HM_TEST_PIPELINE_DESTROY_INJECT_PENDING_PLAYLIST_CALLBACK", "1"},
                {"HM_TEST_RUNTIME_SEEK_READER_DELAY_MS", "400"},
                {"HM_TEST_URI_PLAYLIST_INITIAL_SEEK_FAIL_ONCE", "1"},
                {"HM_TEST_URI_PLAYLIST_INITIAL_SEEK_INTER_SOURCE_DELAY_MS", "250"},
            }),
        "pipeline-app seek process must start with exact-paired multi-chapter sources and native tracker");
    ok &= expect(seek_process.WaitFor("Pipeline running"), "seek pipeline-app must reach PLAYING");
    ok &= expect(
        seek_process.WaitFor(
            "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"session\",\"requestId\":0,\"status\":\"ok\","
            "\"stage\":0,\"generation\":1}"),
        "seek pipeline must announce its initial inspector topology generation");
    const std::string runtime_tuning_response_prefix = "runtime property dsplaytracker0 runtime-tuning-config-file=";
    const std::string runtime_tuning_response = runtime_tuning_response_prefix + playtracker_runtime_config.string();
    const size_t runtime_tuning_mark = seek_process.Mark();
    ok &= expect(
        seek_process.Send(
            "@set-property dsplaytracker0 runtime-tuning-config-file=" + playtracker_runtime_config.string() + "\n"),
        "live zoom tuning command must be delivered");
    ok &= expect(
        seek_process.WaitFor(
            "HSTREAM_RUNTIME_PROPERTY status=captured element=dsplaytracker0 "
            "property=runtime-tuning-config-file original=" +
                playtracker_runtime_config.string(),
            runtime_tuning_mark),
        "backend must capture live tuning before plugin mutation");
    ok &= expect(
        fs::remove(playtracker_runtime_config),
        "UI-owned live tuning file must be removable while plugin mutation is delayed");
    ok &= expect(
        seek_process.WaitFor(runtime_tuning_response, runtime_tuning_mark),
        "captured live zoom tuning must be acknowledged with its original UI path");
    const size_t seek_mark = seek_process.Mark();
    ok &= expect(seek_process.Send("@seek 10000000000 2\n"), "local seek command must be delivered");
    ok &= expect(
        seek_process.WaitFor("HSTREAM_URI_PLAYLIST_CALLBACK status=queued action=switch source=0", seek_mark),
        "seek regression must queue an old-generation physical-boundary callback");
    ok &= expect(
        seek_process.WaitFor("HSTREAM_URI_PLAYLIST_CALLBACK status=cancelled action=switch source=0", seek_mark),
        "recreation must remove a pending physical-boundary callback before handing AppCtx to the worker");
    ok &= expect(
        seek_process.WaitFor("HSTREAM_PIPELINE_READER status=released", seek_mark),
        "recreation must wait for an in-flight pipeline reader before destroying its generation");
    ok &= expect(
        seek_process.WaitFor("Destroy pipeline", seek_mark),
        "pipeline replacement must proceed after the shared reader fence is released");
    ok &= expect(
        seek_process.WaitFor(
            "HSTREAM_URI_PLAYLIST_CALLBACK status=replacement-fenced action=replacement-switch source=0", seek_mark),
        "replacement playlist callbacks must remain non-dispatchable while the worker owns AppCtx");
    ok &= expect(
        seek_process.WaitFor("falling back to exact decoded trimming", seek_mark),
        "a rejected accelerated seek must preserve the decoded-trimming fallback");
    ok &= expect(
        seek_process.WaitFor("URI-playlist source 1 reset NVIDIA decoder after preroll seek", seek_mark),
        "an accelerated hardware-decoder seek must rebuild NVDEC before admitting post-seek pixels");
    const std::string& reader_output = seek_process.output();
    const size_t reader_released = reader_output.find("HSTREAM_PIPELINE_READER status=released", seek_mark);
    const size_t reader_destroy = reader_output.find("Destroy pipeline", seek_mark);
    ok &= expect(
        reader_released != std::string::npos && reader_destroy != std::string::npos && reader_released < reader_destroy,
        "pipeline replacement must begin only after the shared reader fence is released");
    ok &= expect(
        seek_process.WaitFor("HSTREAM_SEEK status=ok generation=2 position_ns=10000000000", seek_mark),
        "local-render-only playback must acknowledge the first processed frame after a replacement seek");
    ok &= expect(
        seek_process.WaitFor(
            "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"session\",\"requestId\":0,\"status\":\"ok\","
            "\"stage\":0,\"generation\":2}",
            seek_mark),
        "successful seek recreation must publish a new inspector topology generation");
    const std::string queued_pre_recreation_command =
        std::string("@inspect-set-property 150 0 1 0 ") + kQueuePath + " " + kSilentProperty + " dHJ1ZQ==\n";
    const size_t stale_seek_inspector_mark = seek_process.Mark();
    ok &= expect(
        seek_process.Send(queued_pre_recreation_command),
        "a property command retained from the pre-recreation graph must be delivered");
    ok &= expect(
        seek_process.WaitFor(
            "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"set-result\",\"requestId\":150,"
            "\"status\":\"error\",\"stage\":0,\"generation\":1,"
            "\"message\":\"Stale pipeline inspector stage/generation",
            stale_seek_inspector_mark),
        "a queued pre-recreation property command must not reach the replacement topology");
    const size_t replacement_graph_mark = seek_process.Mark();
    ok &= expect(
        seek_process.Send("@inspect-pipeline 151 0 2\n"),
        "replacement topology graph request must be delivered with the new generation");
    ok &= expect(
        seek_process.WaitFor(
            "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"graph\",\"requestId\":151,"
            "\"status\":\"ok\",\"stage\":0,\"generation\":2",
            replacement_graph_mark),
        "the newly published inspector generation must accept fresh graph requests");
    ok &= expect(
        seek_process.WaitFor(runtime_tuning_response_prefix, seek_mark),
        "replacement pipeline must restore acknowledged live zoom tuning before completing the seek");
    ok &= expect(
        seek_process.WaitForProgressAtOrBeyond(10, seek_mark, std::chrono::seconds(12)),
        "post-seek progress must come from media at or beyond the requested position");
    const size_t relative_seek_mark = seek_process.Mark();
    ok &= expect(seek_process.Send("@seek-relative 10000000000 3\n"), "relative local seek command must be delivered");
    const std::string relative_response = "HSTREAM_SEEK status=ok generation=3 position_ns=";
    ok &= expect(
        seek_process.WaitFor(relative_response, relative_seek_mark),
        "relative seek must acknowledge a target derived from the live pipeline position");
    const std::string& seek_output = seek_process.output();
    const size_t relative_response_offset = seek_output.find(relative_response, relative_seek_mark);
    const uint64_t relative_position = relative_response_offset == std::string::npos
        ? 0
        : std::strtoull(seek_output.c_str() + relative_response_offset + relative_response.size(), nullptr, 10);
    ok &= expect(
        relative_position >= 20'000'000'000ULL && relative_position <= 1'800'000'000'000ULL,
        "relative +10s seek must use a valid fresh position after the prior 10s absolute seek");

    const uint64_t repeated_targets_ns[] = {
        5'000'000'000ULL,
        35'000'000'000ULL,
        12'000'000'000ULL,
        45'000'000'000ULL,
        8'000'000'000ULL,
    };
    for (uint64_t generation = 4; generation < 9 && ok; ++generation) {
      const uint64_t target_ns = repeated_targets_ns[generation - 4];
      const size_t repeated_seek_mark = seek_process.Mark();
      ok &= expect(
          seek_process.Send("@seek " + std::to_string(target_ns) + " " + std::to_string(generation) + "\n"),
          "repeated seek command must be delivered");
      ok &= expect(
          seek_process.WaitFor(
              "HSTREAM_SEEK status=ok generation=" + std::to_string(generation) +
                  " position_ns=" + std::to_string(target_ns),
              repeated_seek_mark),
          "repeated seek must complete without wedging runtime controls");
    }

    const size_t cross_chapter_seek_mark = seek_process.Mark();
    ok &= expect(seek_process.Send("@seek 1810000000000 9\n"), "cross-chapter seek command must be delivered");
    ok &= expect(
        seek_process.WaitFor(
            "HSTREAM_SEEK status=ok generation=9 position_ns=1810000000000",
            cross_chapter_seek_mark,
            std::chrono::seconds(20)),
        "seek must select the next physical chapter and complete from pristine exact-pair state");
    ok &= expect(
        seek_process.WaitForProgressAtOrBeyond(1810, cross_chapter_seek_mark, std::chrono::seconds(12)),
        "cross-chapter progress must report the achieved logical playlist position");
    const size_t backward_chapter_seek_mark = seek_process.Mark();
    ok &= expect(seek_process.Send("@seek 20000000000 10\n"), "backward chapter seek command must be delivered");
    ok &= expect(
        seek_process.WaitFor(
            "HSTREAM_SEEK status=ok generation=10 position_ns=20000000000",
            backward_chapter_seek_mark,
            std::chrono::seconds(20)),
        "backward seek from chapter two must reset exact-pair and tracker state");
    const size_t final_teardown_mark = seek_process.Mark();
    ok &= expect(seek_process.Send("q"), "quit command must be delivered to seek pipeline-app");
    exit_code = -1;
    ok &= expect(seek_process.WaitForExit(&exit_code), "seek pipeline-app must stop promptly after q");
    ok &= expect(exit_code == 0, "seek pipeline-app must exit successfully");
    ok &= expect(
        seek_process.output().find(
            "HSTREAM_URI_PLAYLIST_CALLBACK status=queued action=switch source=0", final_teardown_mark) !=
            std::string::npos,
        "final teardown regression must queue a callback owned by the last pipeline generation");
    ok &= expect(
        seek_process.output().find(
            "HSTREAM_URI_PLAYLIST_CALLBACK status=cancelled action=switch source=0", final_teardown_mark) !=
            std::string::npos,
        "central pipeline destruction must cancel last-generation URI callbacks before AppCtx release");
  }

  if (ok) {
    ok &= verify_telemetry_seek_rejection(
        argv[1], telemetry_seek_config, telemetry_csv_dir, "canonical telemetry private property");
  }
  if (ok) {
    ok &= verify_telemetry_seek_rejection(
        argv[1], telemetry_alias_seek_config, telemetry_alias_csv_dir, "underscore telemetry private property alias");
  }
  if (ok) {
    ok &= verify_telemetry_seek_rejection(
        argv[1],
        telemetry_whitespace_seek_config,
        telemetry_whitespace_csv_dir,
        "quoted whitespace telemetry private property value",
        root);
  }
  if (ok) {
    ok &= verify_telemetry_seek_allowed_when_disabled(
        argv[1], telemetry_disabled_seek_config, telemetry_disabled_csv_dir);
  }

  PipelineProcess fallback_seek_process;
  if (ok) {
    ok &= expect(
        fallback_seek_process.Start(
            argv[1],
            playlist_seek_config,
            "URI-MULTIPLE",
            "RENDER",
            true,
            {
                {"HM_TEST_URI_PLAYLIST_INITIAL_SEEK_FAIL_ONCE", "1"},
                {"HM_TEST_RUNTIME_SEEK_TRANSITION_TIMEOUT_MS", "150"},
            }),
        "decoded-trimming fallback seek process must start");
    ok &= expect(
        fallback_seek_process.WaitFor("Pipeline running"), "decoded-trimming fallback pipeline must reach PLAYING");
    ok &= expect(
        fallback_seek_process.WaitForProgressAtOrBeyond(1, 0, std::chrono::seconds(12)),
        "decoded-trimming fallback pipeline must process media before seeking");
    const size_t fallback_seek_mark = fallback_seek_process.Mark();
    ok &= expect(
        fallback_seek_process.Send("@seek 10000000000 20\n"),
        "long decoded-trimming fallback seek command must be delivered");
    ok &= expect(
        fallback_seek_process.WaitFor(
            "HSTREAM_SEEK_FALLBACK status=active generation=20 decoded_trim_ns=10000000000 timeout_ms=20000",
            fallback_seek_mark,
            std::chrono::seconds(12)),
        "rejected acceleration must publish a deadline derived from its decoded distance");
    ok &= expect(
        fallback_seek_process.WaitFor(
            "HSTREAM_SEEK status=ok generation=20 position_ns=10000000000",
            fallback_seek_mark,
            std::chrono::seconds(30)),
        "a long healthy decoded-trimming fallback must outlive the ordinary first-frame deadline");
    ok &= expect(fallback_seek_process.Send("q"), "fallback seek quit command must be delivered");
    exit_code = -1;
    ok &= expect(fallback_seek_process.WaitForExit(&exit_code), "decoded-trimming fallback process must stop promptly");
    ok &= expect(exit_code == 0, "decoded-trimming fallback process must exit successfully");
  }

  PipelineProcess multi_track_seek_process;
  PipelineProcess decoder_restart_failure_process;
  if (ok) {
    ok &= expect(
        decoder_restart_failure_process.Start(
            argv[1],
            playlist_seek_config,
            "URI-MULTIPLE",
            "RENDER",
            true,
            {{"HM_TEST_URI_PLAYLIST_INITIAL_SEEK_DECODER_RESTART_FAIL_ONCE", "1"}}),
        "decoder-restart failure seek process must start");
    ok &= expect(
        decoder_restart_failure_process.WaitFor("Pipeline running"),
        "decoder-restart failure pipeline must reach PLAYING");
    const size_t decoder_restart_failure_mark = decoder_restart_failure_process.Mark();
    ok &= expect(
        decoder_restart_failure_process.Send("@seek 10000000000 21\n"),
        "decoder-restart failure seek command must be delivered");
    exit_code = 0;
    ok &= expect(
        decoder_restart_failure_process.WaitForExit(&exit_code, std::chrono::seconds(20)),
        "decoder-restart failure must terminate reconstruction promptly");
    const std::string& decoder_restart_failure_output = decoder_restart_failure_process.output();
    ok &= expect(exit_code != 0, "decoder-restart failure must make pipeline reconstruction fatal");
    ok &= expect(
        decoder_restart_failure_output.find(
            "Could not rebuild NVIDIA decoder after URI-playlist seek", decoder_restart_failure_mark) !=
            std::string::npos,
        "decoder-restart failure must report the fatal reconstruction cause");
    ok &= expect(
        decoder_restart_failure_output.find("falling back to exact decoded trimming", decoder_restart_failure_mark) ==
            std::string::npos,
        "a stopped decoder must never advertise decoded-trimming fallback");
  }

  if (ok) {
    ok &= expect(
        multi_track_seek_process.Start(
            argv[1],
            multi_track_playlist_seek_config,
            "URI-MULTIPLE",
            "RENDER",
            true,
            {
                {"HM_TEST_URI_PLAYLIST_INITIAL_SEEK_INTER_SOURCE_DELAY_MS", "250"},
                {"HM_TEST_URI_PLAYLIST_INITIAL_SEEK_REPORT_RELINK", "1"},
            }),
        "multi-video-track seek process must start");
    ok &= expect(multi_track_seek_process.WaitFor("Pipeline running"), "multi-track pipeline must reach PLAYING");
    const size_t multi_track_seek_mark = multi_track_seek_process.Mark();
    ok &= expect(
        multi_track_seek_process.Send("@seek 10000000000 21\n"),
        "multi-track replacement seek command must be delivered");
    ok &= expect(
        multi_track_seek_process.WaitFor(
            "HSTREAM_URI_PLAYLIST_INITIAL_SEEK status=secondary-ignored source=0 media=video",
            multi_track_seek_mark,
            std::chrono::seconds(12)),
        "initial-seek admission must explicitly ignore a secondary raw video track");
    ok &= expect(
        multi_track_seek_process.WaitFor(
            "HSTREAM_SEEK status=ok generation=21 position_ns=10000000000",
            multi_track_seek_mark,
            std::chrono::seconds(20)),
        "multi-track pad replacement must preserve exact-pair seek admission");
    ok &= expect(multi_track_seek_process.Send("q"), "multi-track seek quit command must be delivered");
    exit_code = -1;
    ok &= expect(multi_track_seek_process.WaitForExit(&exit_code), "multi-track seek process must stop promptly");
    ok &= expect(exit_code == 0, "multi-track seek process must exit successfully");
  }

  PipelineProcess unequal_seek_process;
  if (ok) {
    ok &= expect(
        unequal_seek_process.Start(argv[1], unequal_playlist_seek_config, "URI-MULTIPLE", "RENDER", true),
        "unequal exact-pair seek process must start");
    ok &= expect(unequal_seek_process.WaitFor("Pipeline running"), "unequal exact-pair pipeline must reach PLAYING");
    const size_t end_seek_mark = unequal_seek_process.Mark();
    ok &= expect(
        unequal_seek_process.Send("@seek 1800000000000 12\n"), "exact-pair endpoint seek command must be delivered");
    ok &= expect(
        unequal_seek_process.WaitFor(
            "HSTREAM_SEEK status=ok generation=12 position_ns=1799000000000", end_seek_mark, std::chrono::seconds(20)),
        "endpoint seek must use the shorter exact-paired source and remain before EOS");
    exit_code = -1;
    ok &= expect(
        unequal_seek_process.WaitForExit(&exit_code, std::chrono::seconds(20)),
        "endpoint seek process must complete promptly after rendering the final frames");
    ok &= expect(exit_code == 0, "endpoint seek must complete cleanly instead of becoming a fatal reconstruction");
  }

  PipelineProcess single_seek_process;
  PipelineProcess interrupted_seek_process;
  PipelineProcess unavailable_worker_seek_process;
  PipelineProcess timed_out_seek_process;
  PipelineProcess timed_run_seek_process;
  PipelineProcess callback_race_seek_process;
  PipelineProcess published_interrupt_seek_process;
  PipelineProcess published_timed_seek_process;
  if (ok) {
    ok &= expect(
        timed_out_seek_process.Start(
            argv[1],
            tracker_config,
            "URI",
            "RENDER",
            true,
            {
                {"HM_TEST_RUNTIME_SEEK_TIMEOUT_MS", "150"},
                {"HM_TEST_RUNTIME_SEEK_RECREATE_DELAY_MS", "500"},
                {"HM_TEST_RUNTIME_SEEK_POST_CREATE_DELAY_MS", "1500"},
            }),
        "delayed-recreation seek process must start");
    ok &= expect(
        timed_out_seek_process.WaitFor("Pipeline running"), "delayed-recreation seek pipeline must reach PLAYING");
    const size_t timed_out_seek_mark = timed_out_seek_process.Mark();
    ok &= expect(
        timed_out_seek_process.Send("@seek 10000000000 13\n"), "delayed reconstruction seek command must be delivered");
    ok &= expect(
        timed_out_seek_process.WaitFor(
            "HSTREAM_SEEK status=failed generation=13 reason=pipeline-recreate-timeout",
            timed_out_seek_mark,
            std::chrono::seconds(3)),
        "reconstruction deadline must be dispatched while the worker is still delayed");
    ok &= expect(
        timed_out_seek_process.WaitFor("Recreate pipeline", timed_out_seek_mark, std::chrono::seconds(8)),
        "timed-out worker must still return AppCtx in a valid replacement generation");
    const size_t timed_out_recreated_at =
        timed_out_seek_process.output().find("Recreate pipeline", timed_out_seek_mark);
    ok &= expect(
        timed_out_recreated_at != std::string::npos &&
            timed_out_seek_process.WaitFor(
                "HSTREAM_SEEK_RECOVERY status=ready generation=13", timed_out_recreated_at, std::chrono::seconds(8)),
        "timed-out reconstruction must publish after replacement media makes the AppCtx safe for controls again");
    const std::string& recovery_output = timed_out_seek_process.output();
    const size_t recovery_playing = recovery_output.find("Pipeline running", timed_out_recreated_at);
    const size_t recovery_ready =
        recovery_output.find("HSTREAM_SEEK_RECOVERY status=ready generation=13", timed_out_recreated_at);
    ok &= expect(
        recovery_playing != std::string::npos && recovery_ready != std::string::npos &&
            recovery_playing < recovery_ready,
        "timed-out reconstruction must not re-enable controls before the replacement reaches PLAYING media");
    ok &= expect(
        timed_out_recreated_at != std::string::npos &&
            timed_out_seek_process.WaitFor("Pipeline running", timed_out_recreated_at, std::chrono::seconds(8)),
        "timed-out replacement must resume local playback before accepting more commands");
    const size_t recovered_progress_mark = timed_out_seek_process.Mark();
    ok &= expect(
        timed_out_recreated_at != std::string::npos &&
            timed_out_seek_process.WaitForProgressAtOrBeyond(10, recovered_progress_mark, std::chrono::seconds(8)),
        "replacement performance timer must continue publishing progress after slow reconstruction");
    ok &= expect(timed_out_seek_process.Send("q"), "timed-out seek process quit command must be delivered");
    exit_code = -1;
    ok &= expect(
        timed_out_seek_process.WaitForExit(&exit_code, std::chrono::seconds(12)),
        "timed-out seek process must remain controllable after worker completion");
    ok &= expect(exit_code == 0, "timed-out local seek must leave a cleanly stoppable render pipeline");
  }

  if (ok) {
    ok &= expect(
        timed_run_seek_process.Start(
            argv[1],
            tracker_config,
            "URI",
            "RENDER",
            true,
            {{"HM_TEST_RUNTIME_SEEK_RECREATE_DELAY_MS", "4000"}},
            {"-t=3"}),
        "timed-run reconstruction seek process must start");
    ok &= expect(timed_run_seek_process.WaitFor("Pipeline running"), "timed-run seek pipeline must reach PLAYING");
    const size_t timed_run_seek_mark = timed_run_seek_process.Mark();
    ok &= expect(
        timed_run_seek_process.Send("@seek 1000000000 16\n"),
        "timed-run delayed reconstruction seek command must be delivered");
    ok &= expect(
        timed_run_seek_process.WaitFor(
            "HSTREAM_SEEK_RECREATION status=started generation=16", timed_run_seek_mark, std::chrono::seconds(3)),
        "timed-run seek worker must begin while the old generation can reach its limit");
    ok &= expect(
        timed_run_seek_process.WaitFor(
            "HSTREAM_SEEK status=ok generation=16 position_ns=1000000000",
            timed_run_seek_mark,
            std::chrono::seconds(10)),
        "timed-run limit must not stop or tear down worker-owned AppCtx before replacement publication");
    exit_code = -1;
    ok &= expect(
        timed_run_seek_process.WaitForExit(&exit_code, std::chrono::seconds(10)),
        "timed run must stop cleanly after accounting a fresh replacement epoch");
    ok &= expect(exit_code == 0, "timed-run seek must not self-join, abort, or report pipeline failure");
    const std::string& timed_run_output = timed_run_seek_process.output();
    const size_t timed_seek_completed =
        timed_run_output.find("HSTREAM_SEEK status=ok generation=16 position_ns=1000000000", timed_run_seek_mark);
    const size_t timed_run_success = timed_run_output.find("App run successful", timed_run_seek_mark);
    ok &= expect(
        timed_seek_completed != std::string::npos && timed_run_success != std::string::npos &&
            timed_seek_completed < timed_run_success,
        "timed run must publish reconstruction before reporting final success");
  }

  if (ok) {
    ok &= expect(
        callback_race_seek_process.Start(
            argv[1],
            playlist_seek_config,
            "URI-MULTIPLE",
            "RENDER",
            true,
            {{"HM_TEST_URI_PLAYLIST_SCHEDULE_SUSPEND_RACE", "1"}}),
        "URI callback schedule/suspend race process must start");
    ok &= expect(
        callback_race_seek_process.WaitFor("Pipeline running"),
        "URI callback schedule/suspend race pipeline must reach PLAYING");
    const size_t callback_race_mark = callback_race_seek_process.Mark();
    ok &= expect(
        callback_race_seek_process.Send("@seek 10000000000 17\n"),
        "URI callback schedule/suspend race seek must be delivered");
    ok &= expect(
        callback_race_seek_process.WaitFor(
            "HSTREAM_URI_PLAYLIST_CALLBACK status=race-fenced action=switch source=0",
            callback_race_mark,
            std::chrono::seconds(8)),
        "callback suspension must exclusively claim and destroy a concurrently published GLib source");
    ok &= expect(
        callback_race_seek_process.WaitFor(
            "HSTREAM_SEEK status=ok generation=17 position_ns=10000000000",
            callback_race_mark,
            std::chrono::seconds(20)),
        "pipeline must remain seekable after the schedule/suspend ownership race");
    ok &= expect(callback_race_seek_process.Send("q"), "callback-race seek process quit must be delivered");
    exit_code = -1;
    ok &= expect(callback_race_seek_process.WaitForExit(&exit_code), "callback-race seek process must stop promptly");
    ok &= expect(exit_code == 0, "callback-race seek process must exit successfully");
  }

  if (ok) {
    ok &= expect(
        published_interrupt_seek_process.Start(
            argv[1], tracker_config, "URI", "RENDER", true, {{"HM_TEST_RUNTIME_SEEK_SUPPRESS_FIRST_FRAME_ACK", "1"}}),
        "post-publication interrupt seek process must start");
    ok &= expect(
        published_interrupt_seek_process.WaitFor("Pipeline running"),
        "post-publication interrupt pipeline must reach PLAYING");
    const size_t published_interrupt_mark = published_interrupt_seek_process.Mark();
    ok &= expect(
        published_interrupt_seek_process.Send("@seek 10000000000 18\n"),
        "post-publication interrupt seek must be delivered");
    ok &= expect(
        published_interrupt_seek_process.WaitFor(
            "HSTREAM_SEEK_RECREATION status=published generation=18",
            published_interrupt_mark,
            std::chrono::seconds(12)),
        "SIGINT regression must wait until the replacement is published but still awaiting its first frame");
    ok &= expect(
        published_interrupt_seek_process.Interrupt(),
        "SIGINT must be delivered while the published seek awaits first-frame acknowledgement");
    exit_code = -1;
    ok &= expect(
        published_interrupt_seek_process.WaitForExit(&exit_code, std::chrono::seconds(8)),
        "SIGINT after replacement publication must stop promptly");
    ok &= expect(exit_code == 0, "SIGINT after replacement publication must exit successfully");
    const std::string& output = published_interrupt_seek_process.output();
    const size_t terminal_seek =
        output.find("HSTREAM_SEEK status=failed generation=18 reason=pipeline-stopped", published_interrupt_mark);
    const size_t app_success = output.find("App run successful", published_interrupt_mark);
    ok &= expect(
        terminal_seek != std::string::npos && app_success != std::string::npos && terminal_seek < app_success,
        "SIGINT must terminally publish an accepted waiting-for-frame seek before final success");
  }

  if (ok) {
    ok &= expect(
        published_timed_seek_process.Start(
            argv[1],
            tracker_config,
            "URI",
            "RENDER",
            true,
            {{"HM_TEST_RUNTIME_SEEK_SUPPRESS_FIRST_FRAME_ACK", "1"}},
            {"-t=3"}),
        "post-publication timed-stop seek process must start");
    ok &= expect(
        published_timed_seek_process.WaitFor("Pipeline running"),
        "post-publication timed-stop pipeline must reach PLAYING");
    const size_t published_timed_mark = published_timed_seek_process.Mark();
    ok &= expect(
        published_timed_seek_process.Send("@seek 1000000000 19\n"),
        "post-publication timed-stop seek must be delivered");
    ok &= expect(
        published_timed_seek_process.WaitFor(
            "HSTREAM_SEEK_RECREATION status=published generation=19", published_timed_mark, std::chrono::seconds(12)),
        "timed-stop regression must reach waiting-for-frame after publication");
    exit_code = -1;
    ok &= expect(
        published_timed_seek_process.WaitForExit(&exit_code, std::chrono::seconds(12)),
        "time limit must stop a published seek that is still awaiting its first-frame acknowledgement");
    ok &= expect(exit_code == 0, "timed stop after replacement publication must exit successfully");
    const std::string& output = published_timed_seek_process.output();
    const size_t terminal_seek =
        output.find("HSTREAM_SEEK status=failed generation=19 reason=pipeline-stopped", published_timed_mark);
    const size_t app_success = output.find("App run successful", published_timed_mark);
    ok &= expect(
        terminal_seek != std::string::npos && app_success != std::string::npos && terminal_seek < app_success,
        "timed stop must terminally publish an accepted waiting-for-frame seek before final success");
  }

  if (ok) {
    ok &= expect(
        unavailable_worker_seek_process.Start(
            argv[1], tracker_config, "URI", "RENDER", true, {{"HM_TEST_RUNTIME_SEEK_WORKER_UNAVAILABLE", "1"}}),
        "worker-unavailable seek process must start");
    ok &= expect(
        unavailable_worker_seek_process.WaitFor("Pipeline running"),
        "worker-unavailable seek pipeline must reach PLAYING");
    const size_t unavailable_worker_mark = unavailable_worker_seek_process.Mark();
    ok &= expect(
        unavailable_worker_seek_process.Send("@seek 10000000000 15\n"),
        "worker-unavailable seek command must be delivered");
    ok &= expect(
        unavailable_worker_seek_process.WaitFor(
            "HSTREAM_SEEK status=failed generation=15 reason=pipeline-recreate-worker-unavailable",
            unavailable_worker_mark,
            std::chrono::seconds(3)),
        "worker construction failure must complete the public seek transaction");
    exit_code = 0;
    ok &= expect(
        unavailable_worker_seek_process.WaitForExit(&exit_code, std::chrono::seconds(8)),
        "worker construction failure must discard and stop the fenced local generation promptly");
    ok &= expect(exit_code != 0, "worker construction failure must not claim a healthy playback process");
    ok &= expect(
        unavailable_worker_seek_process.output().find("Pipeline EOS finalization timed out") == std::string::npos,
        "worker construction failure must not enter local-render EOS finalization timeout");
  }

  if (ok) {
    ok &= expect(
        interrupted_seek_process.Start(
            argv[1], tracker_config, "URI", "RENDER", true, {{"HM_TEST_RUNTIME_SEEK_RECREATE_DELAY_MS", "2000"}}),
        "interrupt-during-recreation seek process must start");
    ok &= expect(
        interrupted_seek_process.WaitFor("Pipeline running"),
        "interrupt-during-recreation seek pipeline must reach PLAYING");
    const size_t interrupted_seek_mark = interrupted_seek_process.Mark();
    ok &= expect(
        interrupted_seek_process.Send("@seek 10000000000 14\n"),
        "interrupt-during-recreation seek command must be delivered");
    ok &= expect(
        interrupted_seek_process.WaitFor(
            "HSTREAM_SEEK_RECREATION status=started generation=14", interrupted_seek_mark, std::chrono::seconds(3)),
        "seek worker must own AppCtx before interruption");
    ok &= expect(interrupted_seek_process.Interrupt(), "SIGINT must be delivered while seek recreation is active");
    exit_code = -1;
    ok &= expect(
        interrupted_seek_process.WaitForExit(&exit_code, std::chrono::seconds(8)),
        "SIGINT during seek recreation must stop promptly after the worker returns");
    ok &= expect(exit_code == 0, "SIGINT during local-render recreation must exit successfully");
    ok &= expect(
        interrupted_seek_process.output().find("Pipeline EOS finalization timed out") == std::string::npos,
        "discarded local-render replacement must not enter EOS finalization timeout");
    ok &= expect(
        interrupted_seek_process.output().find("INTERNAL: App run failed") == std::string::npos,
        "SIGINT during local-render recreation must not report an internal failure");
  }

  if (ok) {
    ok &= expect(
        single_seek_process.Start(argv[1], tracker_config, "URI", "RENDER", true),
        "single-URI native-tracker seek process must start");
    ok &= expect(single_seek_process.WaitFor("Pipeline running"), "single-URI seek pipeline must reach PLAYING");
    const size_t single_seek_mark = single_seek_process.Mark();
    ok &= expect(single_seek_process.Send("@seek 10000000000 11\n"), "single-URI seek command must be delivered");
    ok &= expect(
        single_seek_process.WaitFor(
            "HSTREAM_SEEK status=ok generation=11 position_ns=10000000000", single_seek_mark, std::chrono::seconds(20)),
        "single URI must use pre-preroll decoded-pad positioning instead of a PAUSED flushing seek");
    ok &= expect(single_seek_process.Send("q"), "single-URI quit command must be delivered");
    exit_code = -1;
    ok &= expect(single_seek_process.WaitForExit(&exit_code), "single-URI seek process must stop promptly after q");
    ok &= expect(exit_code == 0, "single-URI seek process must exit successfully");
  }

  PipelineProcess failed_process;
  PipelineProcess archive_process;
  PipelineProcess destroyed_recreation_process;
  PipelineProcess partial_recreation_process;
  if (ok) {
    ok &= expect(
        archive_process.Start(
            argv[1],
            recreate_config,
            "URI",
            "ENCODE_FILE",
            false,
            {{"HM_TEST_VERIFY_PIPELINE_RECREATE_SOURCE_CLEANUP", "1"}}),
        "pipeline-app archive process must start");
    ok &= expect(archive_process.WaitFor("Pipeline running"), "archive pipeline must reach PLAYING");
    ok &= expect(
        archive_process.WaitFor(
            "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"session\",\"requestId\":0,\"status\":\"ok\","
            "\"stage\":0,\"generation\":1}"),
        "periodic-recreation pipeline must announce its initial topology generation");
    const size_t recreate_mark = archive_process.Mark();
    ok &= expect(
        archive_process.WaitFor("Recreate pipeline", recreate_mark, std::chrono::seconds(12)),
        "archive pipeline must recreate before the final user stop");
    const size_t recreated_at = archive_process.output().find("Recreate pipeline", recreate_mark);
    ok &= expect(
        recreated_at != std::string::npos &&
            archive_process.WaitFor("Pipeline running", recreated_at, std::chrono::seconds(12)),
        "recreated archive pipeline must return to PLAYING");
    ok &= expect(
        recreated_at != std::string::npos &&
            archive_process.WaitFor(
                "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"session\",\"requestId\":0,"
                "\"status\":\"ok\",\"stage\":0,\"generation\":2}",
                recreated_at,
                std::chrono::seconds(12)),
        "periodic pipeline replacement must publish a new inspector topology generation");
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    ok &= expect(archive_process.Interrupt(), "archive pipeline SIGINT stop must be delivered");
    exit_code = -1;
    ok &= expect(archive_process.WaitForExit(&exit_code), "archive pipeline must stop promptly after SIGINT");
    ok &= expect(exit_code == 0, "archive pipeline must exit successfully after EOS finalization");
    ok &= expect(
        archive_process.output().find("HSTREAM_PIPELINE_RECREATE_TIMER status=cancelled") != std::string::npos,
        "stage cleanup must remove its repeating pipeline recreation source before AppCtx destruction");
    ok &= expect(fs::is_regular_file(archive) && fs::file_size(archive) > 0, "archive output must be written");
    ok &= expect(run_command({"ffprobe", "-v", "error", archive.string()}), "user-stopped archive must be playable");
  }

  const auto verify_failed_recreation_generation = [&](PipelineProcess* recreation_process,
                                                       const char* injected_environment,
                                                       const char* failure_phase) {
    if (!ok) {
      return;
    }
    ok &= expect(
        recreation_process->Start(argv[1], recreate_config, "URI", "FAKE", false, {{injected_environment, "1"}}),
        "injected-failure periodic recreation process must start");
    ok &= expect(
        recreation_process->WaitFor("Pipeline running"),
        "injected-failure periodic recreation must reach initial PLAYING");
    ok &= expect(
        recreation_process->WaitFor(
            "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"session\",\"requestId\":0,\"status\":\"ok\","
            "\"stage\":0,\"generation\":1}"),
        "injected-failure recreation must announce its initial inspector generation");
    const size_t recreation_mark = recreation_process->Mark();
    const std::string failure_marker =
        std::string("HSTREAM_PIPELINE_RECREATE status=injected-failure phase=") + failure_phase;
    ok &= expect(
        recreation_process->WaitFor(failure_marker, recreation_mark, std::chrono::seconds(12)),
        "periodic recreation must reach the requested post-destruction failure phase");
    ok &= expect(
        recreation_process->WaitFor(
            "HSTREAM_PIPELINE_INSPECTOR {\"version\":1,\"kind\":\"session\",\"requestId\":0,\"status\":\"ok\","
            "\"stage\":0,\"generation\":2}",
            recreation_mark),
        "a failed recreation must invalidate the destroyed inspector generation before exiting");
    ok &= expect(
        recreation_process->WaitFor(
            "HSTREAM_PIPELINE_RECREATE status=failed reason=periodic-reconstruction",
            recreation_mark,
            std::chrono::seconds(12)),
        "a failed periodic recreation must report a terminal reconstruction failure");
    int recreation_exit_code = -1;
    bool recreation_exited_normally = false;
    ok &= expect(
        recreation_process->WaitForExit(&recreation_exit_code, std::chrono::seconds(12), &recreation_exited_normally),
        "injected-failure recreation process must stop promptly");
    ok &= expect(recreation_exited_normally, "injected-failure periodic recreation must exit normally");
    ok &= expect(recreation_exit_code != 0, "injected-failure periodic recreation must exit nonzero");
    ok &= expect(
        recreation_process->output().find("GStreamer-CRITICAL", recreation_mark) == std::string::npos,
        "failed periodic recreation cleanup must not access destroyed GStreamer objects");
    ok &= expect(
        recreation_process->output().find("Could not find 'sink' in 'sink_bin'", recreation_mark) == std::string::npos,
        "failed periodic recreation cleanup must not inspect stale sink bins");
  };

  verify_failed_recreation_generation(
      &destroyed_recreation_process, "HM_TEST_PIPELINE_RECREATE_FAIL_AFTER_DESTROY", "after-destroy");
  verify_failed_recreation_generation(
      &partial_recreation_process, "HM_TEST_PIPELINE_RECREATE_FAIL_DURING_CREATE", "during-create");

  if (ok) {
    ok &=
        expect(failed_process.Start(argv[1], error_config, "URI-MULTIPLE"), "pipeline-app failure process must start");
    exit_code = 0;
    ok &= expect(
        failed_process.WaitForExit(&exit_code, std::chrono::seconds(20)),
        "pipeline-app must stop after a decoder playlist error");
    ok &= expect(exit_code != 0, "pipeline-app must return nonzero after a decoder playlist error");
    ok &= expect(
        failed_process.output().find("App run successful") == std::string::npos,
        "pipeline-app must never report success after a decoder playlist error");
  }

  if (!ok) {
    process.DumpOutput();
    if (!relaunched_process.output().empty()) {
      relaunched_process.DumpOutput();
    }
    if (!seek_process.output().empty()) {
      seek_process.DumpOutput();
    }
    if (!fallback_seek_process.output().empty()) {
      fallback_seek_process.DumpOutput();
    }
    if (!multi_track_seek_process.output().empty()) {
      multi_track_seek_process.DumpOutput();
    }
    if (!decoder_restart_failure_process.output().empty()) {
      decoder_restart_failure_process.DumpOutput();
    }
    if (!unequal_seek_process.output().empty()) {
      unequal_seek_process.DumpOutput();
    }
    if (!single_seek_process.output().empty()) {
      single_seek_process.DumpOutput();
    }
    if (!timed_out_seek_process.output().empty()) {
      timed_out_seek_process.DumpOutput();
    }
    if (!timed_run_seek_process.output().empty()) {
      timed_run_seek_process.DumpOutput();
    }
    if (!callback_race_seek_process.output().empty()) {
      callback_race_seek_process.DumpOutput();
    }
    if (!interrupted_seek_process.output().empty()) {
      interrupted_seek_process.DumpOutput();
    }
    if (!unavailable_worker_seek_process.output().empty()) {
      unavailable_worker_seek_process.DumpOutput();
    }
    if (!failed_process.output().empty()) {
      failed_process.DumpOutput();
    }
    if (!archive_process.output().empty()) {
      archive_process.DumpOutput();
    }
    if (!destroyed_recreation_process.output().empty()) {
      destroyed_recreation_process.DumpOutput();
    }
    if (!partial_recreation_process.output().empty()) {
      partial_recreation_process.DumpOutput();
    }
    std::cerr << "fixture retained at " << root << '\n';
  } else {
    fs::remove_all(root);
  }
  return ok ? 0 : 1;
}
