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
      bool headless_render_video = false) {
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
    bool recreate_pipeline = false) {
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
  const fs::path recreate_config = root / "pipeline-recreate.yaml";
  const fs::path error_config = root / "pipeline-error.yaml";
  const fs::path archive = root / "archive.mkv";
  ok &= expect(
      run_command({
          "ffmpeg",    "-hide_banner", "-loglevel",
          "error",     "-y",           "-f",
          "lavfi",     "-i",           "testsrc2=size=256x144:rate=15,format=yuv420p",
          "-t",        "300",          "-an",
          "-c:v",      "libx264",      "-preset",
          "ultrafast", "-g",           "15",
          "-pix_fmt",  "yuv420p",      video.string(),
      }),
      "synthetic seekable input must be generated");
  ok &= expect(write_config(config, video, archive), "pipeline-app test config must be written");
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
        seek_process.Start(argv[1], config, "URI", "RENDER", true),
        "pipeline-app seek process must start with a headless local-render sink");
    ok &= expect(seek_process.WaitFor("Pipeline running"), "seek pipeline-app must reach PLAYING");
    const size_t seek_mark = seek_process.Mark();
    ok &= expect(seek_process.Send("@seek 10000000000 2\n"), "local seek command must be delivered");
    ok &= expect(
        seek_process.WaitFor("HSTREAM_SEEK status=ok generation=2 position_ns=10000000000", seek_mark),
        "local-render-only playback must acknowledge an accurate flushing seek after it completes");
    ok &= expect(seek_process.Send("q"), "quit command must be delivered to seek pipeline-app");
    exit_code = -1;
    ok &= expect(seek_process.WaitForExit(&exit_code), "seek pipeline-app must stop promptly after q");
    ok &= expect(exit_code == 0, "seek pipeline-app must exit successfully");
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
