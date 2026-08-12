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

  bool Start(const fs::path& executable, const fs::path& config) {
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
          "--enable-sources=URI",
          "--enable-sinks=FAKE",
      };
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

bool write_config(const fs::path& config, const fs::path& video) {
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
         << "sink0:\n"
         << "  enable: 1\n"
         << "  sink-id: 0\n"
         << "  type: 1\n"
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
  ok &= expect(
      run_command({
          "ffmpeg",    "-hide_banner", "-loglevel",
          "error",     "-y",           "-f",
          "lavfi",     "-i",           "testsrc2=size=256x144:rate=15,format=yuv420p",
          "-t",        "30",           "-an",
          "-c:v",      "libx264",      "-preset",
          "ultrafast", "-g",           "15",
          "-pix_fmt",  "yuv420p",      video.string(),
      }),
      "synthetic seekable input must be generated");
  ok &= expect(write_config(config, video), "pipeline-app test config must be written");

  PipelineProcess process;
  ok &= expect(process.Start(argv[1], config), "hstream-cli process must start");
  ok &= expect(process.WaitFor("Pipeline running"), "pipeline-app must reach PLAYING");
  ok &= expect(process.running(), "pipeline-app must keep processing after reaching PLAYING");
  ok &= expect(
      process.WaitForProgressAtOrBeyond(1, 0, std::chrono::seconds(12)),
      "pipeline-app must advance its video position before controls");

  for (int iteration = 0; iteration < 3 && ok; ++iteration) {
    size_t mark = process.Mark();
    ok &= expect(process.Send("p"), "pause command must be delivered");
    ok &= expect(process.WaitFor("Pipeline paused", mark), "pipeline-app must reach PAUSED");
    mark = process.Mark();
    ok &= expect(process.Send("r"), "resume command must be delivered");
    ok &= expect(process.WaitFor("Pipeline running", mark), "pipeline-app must resume PLAYING");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }
  size_t mark = process.Mark();
  ok &= expect(process.Send("p"), "pre-seek pause command must be delivered");
  ok &= expect(process.WaitFor("Pipeline paused", mark), "pipeline-app must pause before seek");
  mark = process.Mark();
  ok &= expect(process.Send("@seek 5\n"), "seek command must be delivered");
  ok &= expect(
      process.WaitFor("runtime seek position=0:00:05.000000000", mark),
      "pipeline-app must acknowledge paused runtime seek");
  mark = process.Mark();
  ok &= expect(process.Send("r"), "post-seek resume command must be delivered");
  ok &= expect(process.WaitFor("Pipeline running", mark), "pipeline-app must resume after seek");
  mark = process.Mark();
  ok &= expect(
      process.WaitForProgressAtOrBeyond(5, mark, std::chrono::seconds(12)),
      "pipeline-app must reach the requested video position after paused seek and resume");
  mark = process.Mark();
  ok &= expect(process.Send("@seek 9\n"), "playing seek command must be delivered");
  ok &= expect(
      process.WaitFor("runtime seek position=0:00:09.000000000", mark),
      "pipeline-app must acknowledge runtime seek during continuous playback");
  mark = process.Mark();
  ok &= expect(
      process.WaitForProgressAtOrBeyond(9, mark, std::chrono::seconds(12)),
      "pipeline-app must reach the requested video position after seek during playback");
  ok &= expect(process.running(), "pipeline-app must keep processing after repeated pause/resume and seek");
  ok &= expect(process.Interrupt(), "SIGINT stop must be delivered");
  int exit_code = -1;
  ok &= expect(process.WaitForExit(&exit_code), "pipeline-app must stop promptly after SIGINT");
  ok &= expect(exit_code == 0, "pipeline-app must exit successfully after a user stop");
  ok &= expect(
      process.output().find("App run successful") != std::string::npos,
      "pipeline-app must report successful completion after a user stop");

  PipelineProcess relaunched_process;
  if (ok) {
    ok &= expect(relaunched_process.Start(argv[1], config), "pipeline-app must relaunch after a clean stop");
    ok &= expect(relaunched_process.WaitFor("Pipeline running"), "relaunched pipeline-app must reach PLAYING");
    ok &= expect(
        relaunched_process.WaitForProgressAtOrBeyond(1, 0, std::chrono::seconds(12)),
        "relaunched pipeline-app must advance its video position");
    ok &= expect(relaunched_process.Send("q"), "quit command must be delivered to relaunched pipeline-app");
    exit_code = -1;
    ok &= expect(relaunched_process.WaitForExit(&exit_code), "relaunched pipeline-app must stop promptly after q");
    ok &= expect(exit_code == 0, "relaunched pipeline-app must exit successfully");
    ok &= expect(
        relaunched_process.output().find("App run successful") != std::string::npos,
        "relaunched pipeline-app must report successful completion");
  }

  if (!ok) {
    process.DumpOutput();
    if (!relaunched_process.output().empty()) {
      relaunched_process.DumpOutput();
    }
    std::cerr << "fixture retained at " << root << '\n';
  } else {
    fs::remove_all(root);
  }
  return ok ? 0 : 1;
}
