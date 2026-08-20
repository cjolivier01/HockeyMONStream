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
      const std::vector<std::pair<std::string, std::string>>& environment = {}) {
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
      for (const auto& [name, value] : environment) {
        ::setenv(name.c_str(), value.c_str(), 1);
      }
      std::vector<std::string> arguments = {
          executable.string(),
          "-c",
          config.string(),
          "--enable-sources=" + source_type,
          "--enable-sinks=" + sink_type,
      };
      if (headless_render_video) {
        arguments.push_back("--headless-render-video");
      }
      std::vector<char*> argv;
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

  bool WaitForExit(int* exit_code, std::chrono::seconds timeout = std::chrono::seconds(8)) {
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
    int second_source_chapter_count = 2) {
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
  const fs::path video = root / "input.mp4";
  const fs::path config = root / "pipeline.yaml";
  const fs::path tracker_config = root / "pipeline-tracker.yaml";
  const fs::path playlist_seek_config = root / "pipeline-playlist-seek.yaml";
  const fs::path unequal_playlist_seek_config = root / "pipeline-playlist-unequal-seek.yaml";
  const fs::path playtracker_config = root / "playtracker.yaml";
  const fs::path playtracker_runtime_config = root / "playtracker-runtime.yaml";
  const fs::path recreate_config = root / "pipeline-recreate.yaml";
  const fs::path error_config = root / "pipeline-error.yaml";
  const fs::path archive = root / "archive.mkv";
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
      write_playlist_seek_config(unequal_playlist_seek_config, video, playtracker_config, 1),
      "pipeline-app unequal exact-pair seek config must be written");
  ok &=
      expect(write_config(recreate_config, video, archive, true), "pipeline-app recreate test config must be written");
  ok &= expect(
      write_playlist_error_config(error_config, video, root / "missing-second-chapter.mp4"),
      "pipeline-app error test config must be written");

  PipelineProcess process;
  ok &= expect(process.Start(argv[1], config), "hstream-cli process must start");
  ok &= expect(process.WaitFor("Pipeline running"), "pipeline-app must reach PLAYING");
  ok &= expect(process.running(), "pipeline-app must keep processing after reaching PLAYING");
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
            {{"HM_TEST_RUNTIME_PROPERTY_APPLY_DELAY_MS", "400"}}),
        "pipeline-app seek process must start with exact-paired multi-chapter sources and native tracker");
    ok &= expect(seek_process.WaitFor("Pipeline running"), "seek pipeline-app must reach PLAYING");
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
        seek_process.WaitFor("HSTREAM_SEEK status=ok generation=2 position_ns=10000000000", seek_mark),
        "local-render-only playback must acknowledge the first processed frame after a replacement seek");
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
    ok &= expect(seek_process.Send("q"), "quit command must be delivered to seek pipeline-app");
    exit_code = -1;
    ok &= expect(seek_process.WaitForExit(&exit_code), "seek pipeline-app must stop promptly after q");
    ok &= expect(exit_code == 0, "seek pipeline-app must exit successfully");
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
  PipelineProcess timed_out_seek_process;
  if (ok) {
    ok &= expect(
        timed_out_seek_process.Start(
            argv[1],
            tracker_config,
            "URI",
            "RENDER",
            true,
            {{"HM_TEST_RUNTIME_SEEK_TIMEOUT_MS", "150"}, {"HM_TEST_RUNTIME_SEEK_RECREATE_DELAY_MS", "500"}}),
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
        "timed-out reconstruction must publish when the AppCtx is safe for runtime controls again");
    ok &= expect(
        timed_out_recreated_at != std::string::npos &&
            timed_out_seek_process.WaitFor("Pipeline running", timed_out_recreated_at, std::chrono::seconds(8)),
        "timed-out replacement must resume local playback before accepting more commands");
    ok &= expect(timed_out_seek_process.Send("q"), "timed-out seek process quit command must be delivered");
    exit_code = -1;
    ok &= expect(
        timed_out_seek_process.WaitForExit(&exit_code, std::chrono::seconds(12)),
        "timed-out seek process must remain controllable after worker completion");
    ok &= expect(exit_code == 0, "timed-out local seek must leave a cleanly stoppable render pipeline");
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
  if (ok) {
    ok &= expect(
        archive_process.Start(argv[1], recreate_config, "URI", "ENCODE_FILE"),
        "pipeline-app archive process must start");
    ok &= expect(archive_process.WaitFor("Pipeline running"), "archive pipeline must reach PLAYING");
    const size_t recreate_mark = archive_process.Mark();
    ok &= expect(
        archive_process.WaitFor("Recreate pipeline", recreate_mark, std::chrono::seconds(12)),
        "archive pipeline must recreate before the final user stop");
    const size_t recreated_at = archive_process.output().find("Recreate pipeline", recreate_mark);
    ok &= expect(
        recreated_at != std::string::npos &&
            archive_process.WaitFor("Pipeline running", recreated_at, std::chrono::seconds(12)),
        "recreated archive pipeline must return to PLAYING");
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    ok &= expect(archive_process.Interrupt(), "archive pipeline SIGINT stop must be delivered");
    exit_code = -1;
    ok &= expect(archive_process.WaitForExit(&exit_code), "archive pipeline must stop promptly after SIGINT");
    ok &= expect(exit_code == 0, "archive pipeline must exit successfully after EOS finalization");
    ok &= expect(fs::is_regular_file(archive) && fs::file_size(archive) > 0, "archive output must be written");
    ok &= expect(run_command({"ffprobe", "-v", "error", archive.string()}), "user-stopped archive must be playable");
  }

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
    if (!unequal_seek_process.output().empty()) {
      unequal_seek_process.DumpOutput();
    }
    if (!single_seek_process.output().empty()) {
      single_seek_process.DumpOutput();
    }
    if (!timed_out_seek_process.output().empty()) {
      timed_out_seek_process.DumpOutput();
    }
    if (!failed_process.output().empty()) {
      failed_process.DumpOutput();
    }
    if (!archive_process.output().empty()) {
      archive_process.DumpOutput();
    }
    std::cerr << "fixture retained at " << root << '\n';
  } else {
    fs::remove_all(root);
  }
  return ok ? 0 : 1;
}
