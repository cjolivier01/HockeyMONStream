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
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr auto kStartupTimeout = std::chrono::seconds(120);
constexpr auto kBoundaryStallTimeout = std::chrono::seconds(20);
constexpr auto kHardTimeout = std::chrono::minutes(30);

int chapter_boundary_seconds() {
  const char* value = std::getenv("HSTREAM_CHAPTER_BOUNDARY_SECONDS");
  if (!value || !*value) {
    return 1799;
  }
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  return end && *end == '\0' && parsed > 10 && parsed < 86400 ? static_cast<int>(parsed) : 1799;
}

class PipelineProcess {
 public:
  ~PipelineProcess() {
    StopImmediately();
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
      const fs::path& plugin,
      const fs::path& game_root,
      const fs::path& output_root,
      const std::string& game_id) {
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

      const passwd* user = ::getpwuid(::getuid());
      if (user && user->pw_dir) {
        ::setenv("HOME", user->pw_dir, 1);
      }
      const std::string plugin_path = plugin.parent_path().string();
      ::setenv("GST_PLUGIN_PATH", plugin_path.c_str(), 1);
      ::setenv("HM_GAME_DIR", game_root.c_str(), 1);
      ::setenv("HM_OUTPUT_WORK_DIR", output_root.c_str(), 1);
      ::setenv("HSTREAM_RENDER_AUDIO_MUTED", "1", 1);
      ::setenv("HSTREAM_RUNTIME_ENV_READY", "1", 1);
      ::setenv("USE_NEW_NVSTREAMMUX", "yes", 1);
      if (::chdir(config.parent_path().parent_path().c_str()) != 0) {
        _exit(126);
      }

      std::vector<std::string> arguments = {
          executable.string(),
          "-g",
          game_id,
          "-c",
          config.string(),
          "--enable-sources=URI-MULTIPLE",
          "--enable-sinks=ENCODE_FILE",
          "--options=pipeline.streammux.batch-size=2,pipeline.streammux.sync-inputs=0,"
          "pipeline.streammux.batched-push-timeout=2147483647,"
          "pipeline.streammux.frame-num-reset-on-stream-reset=0,pipeline.streammux.frame-num-reset-on-eos=0,"
          "pipeline.hmstitcher.show=0",
          "--options=pipeline.hmaudio.enable=1",
      };
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
    // Keep the write end open so the CLI's runtime-control reader does not observe EOF during the test.
    input_ = input_pipe[1];
    output_ = output_pipe[0];
    const int flags = ::fcntl(output_, F_GETFL, 0);
    if (flags < 0 || ::fcntl(output_, F_SETFL, flags | O_NONBLOCK) != 0) {
      return false;
    }
    last_progress_at_ = std::chrono::steady_clock::now();
    return pid_ > 0;
  }

  bool WaitFor(const std::string& text, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      Poll();
      if (output_text_.find(text) != std::string::npos) {
        return true;
      }
      if (!running()) {
        return false;
      }
    }
    Poll();
    return output_text_.find(text) != std::string::npos;
  }

  void Poll() {
    Drain();
    if (pid_ <= 0) {
      return;
    }
    int status = 0;
    const pid_t result = ::waitpid(pid_, &status, WNOHANG);
    if (result == pid_) {
      pid_ = -1;
      exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      Drain();
      return;
    }
    pollfd fd{output_, POLLIN, 0};
    ::poll(&fd, 1, 100);
    Drain();
  }

  void StopImmediately() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
      pid_ = -1;
    }
  }

  bool running() const {
    return pid_ > 0;
  }

  int exit_code() const {
    return exit_code_;
  }

  int max_progress_seconds() const {
    return max_progress_seconds_;
  }

  std::chrono::steady_clock::time_point last_progress_at() const {
    return last_progress_at_;
  }

  std::string output_tail(size_t maximum_bytes = 24000) const {
    const size_t begin = output_text_.size() > maximum_bytes ? output_text_.size() - maximum_bytes : 0;
    return output_text_.substr(begin);
  }

 private:
  void ScanProgress() {
    size_t position = progress_scan_position_ > 32 ? progress_scan_position_ - 32 : 0;
    while ((position = output_text_.find("Video ", position)) != std::string::npos) {
      int hours = 0;
      int minutes = 0;
      int seconds = 0;
      if (std::sscanf(output_text_.c_str() + position, "Video %d:%d:%d", &hours, &minutes, &seconds) == 3) {
        const int parsed_seconds = hours * 3600 + minutes * 60 + seconds;
        if (parsed_seconds > max_progress_seconds_) {
          max_progress_seconds_ = parsed_seconds;
          last_progress_at_ = std::chrono::steady_clock::now();
        }
      }
      position += 6;
    }
    progress_scan_position_ = output_text_.size();
  }

  void Drain() {
    char buffer[8192];
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
    ScanProgress();
  }

  pid_t pid_{-1};
  int input_{-1};
  int output_{-1};
  int exit_code_{-1};
  int max_progress_seconds_{-1};
  size_t progress_scan_position_{0};
  std::chrono::steady_clock::time_point last_progress_at_{};
  std::string output_text_;
};

bool link_fixture(const fs::path& source_game, const fs::path& test_game) {
  std::error_code ec;
  fs::create_directories(test_game, ec);
  if (ec) {
    return false;
  }
  fs::copy_file(source_game / "config.yaml", test_game / "config.yaml", fs::copy_options::overwrite_existing, ec);
  if (ec) {
    return false;
  }
  for (const char* camera : {"cam1", "cam2"}) {
    const fs::path source_camera = source_game / camera;
    const fs::path test_camera = test_game / camera;
    fs::create_directory(test_camera, ec);
    if (ec) {
      return false;
    }
    for (fs::directory_iterator it(source_camera, ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file()) {
        continue;
      }
      fs::create_symlink(fs::absolute(it->path()), test_camera / it->path().filename(), ec);
      if (ec) {
        return false;
      }
    }
    if (ec) {
      return false;
    }
  }
  const std::set<std::string> artifacts = {
      "autooptimiser_out.pto",
      "hm_project.pto",
      "left.png",
      "mapping_0000.tif",
      "mapping_0000_x.tif",
      "mapping_0000_y.tif",
      "mapping_0001.tif",
      "mapping_0001_x.tif",
      "mapping_0001_y.tif",
      "panorama.tif",
      "right.png",
      "rink_mask_0.png",
      "s.png",
      "seam_file.png",
  };
  for (const std::string& name : artifacts) {
    if (!fs::is_regular_file(source_game / name)) {
      continue;
    }
    fs::create_symlink(fs::absolute(source_game / name), test_game / name, ec);
    if (ec) {
      return false;
    }
  }
  return true;
}

fs::path create_temp_directory(const fs::path& parent, const std::string& prefix) {
  std::error_code ec;
  fs::create_directories(parent, ec);
  if (ec) {
    return {};
  }
  std::string pattern = (parent / (prefix + "-XXXXXX")).string();
  return ::mkdtemp(pattern.data()) ? fs::path(pattern) : fs::path();
}

fs::path find_archive(const fs::path& output_root) {
  std::error_code ec;
  for (fs::recursive_directory_iterator it(output_root, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file() && it->path().extension() == ".mkv") {
      return it->path();
    }
  }
  return {};
}

} // namespace

int main(int argc, char** argv) {
  ::signal(SIGPIPE, SIG_IGN);
  if (argc != 4) {
    std::cerr << "FAIL: expected hstream-cli, videoprep plugin, and pipeline config paths\n";
    return 1;
  }

  const char* fixture_text = std::getenv("HSTREAM_CHAPTER_FIXTURE_ROOT");
  const passwd* user = ::getpwuid(::getuid());
  const fs::path source_game = fixture_text && *fixture_text ? fs::path(fixture_text)
      : user && user->pw_dir                                 ? fs::path(user->pw_dir) / "Videos" / "tv-12-1-r2"
                                                             : fs::path();
  if (!fs::is_directory(source_game / "cam1") || !fs::is_directory(source_game / "cam2")) {
    std::cerr << "FAIL: chapter fixture is unavailable at " << source_game << '\n';
    return 1;
  }

  const fs::path fixture_root = create_temp_directory(fs::temp_directory_path(), "pipeline-chapter-stall-fixture");
  const char* output_parent_text = std::getenv("HSTREAM_CHAPTER_OUTPUT_ROOT");
  const fs::path output_parent =
      output_parent_text && *output_parent_text ? fs::path(output_parent_text) : fs::temp_directory_path();
  const fs::path output_root = create_temp_directory(output_parent, "pipeline-chapter-stall-output");
  const std::string game_id = source_game.filename().string();
  const fs::path test_game = fixture_root / game_id;
  if (fixture_root.empty() || output_root.empty() || !link_fixture(source_game, test_game)) {
    std::cerr << "FAIL: unable to construct isolated production fixture\n";
    return 1;
  }

  PipelineProcess process;
  if (!process.Start(
          fs::canonical(argv[1]), fs::canonical(argv[3]), fs::canonical(argv[2]), fixture_root, output_root, game_id) ||
      !process.WaitFor("Pipeline running", kStartupTimeout)) {
    std::cerr << "FAIL: production pipeline did not reach PLAYING\n" << process.output_tail() << '\n';
    std::cerr << "Fixture retained at " << fixture_root << " and output at " << output_root << '\n';
    return 1;
  }

  bool crossed_boundary = false;
  const int boundary_seconds = chapter_boundary_seconds();
  const int required_progress_seconds = boundary_seconds + 5;
  const auto deadline = std::chrono::steady_clock::now() + kHardTimeout;
  auto next_status = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    process.Poll();
    const int progress = process.max_progress_seconds();
    if (!process.running()) {
      std::cerr << "FAIL: production pipeline exited before crossing the chapter boundary; exit=" << process.exit_code()
                << " progress=" << progress << "s\n"
                << process.output_tail() << '\n';
      break;
    }
    if (progress >= required_progress_seconds) {
      crossed_boundary = true;
      break;
    }
    if (progress >= boundary_seconds - 10 &&
        std::chrono::steady_clock::now() - process.last_progress_at() >= kBoundaryStallTimeout) {
      std::cerr << "FAIL: full production program/archive progress stopped at the first camera chapter boundary; "
                << "last video second=" << progress << "\n"
                << process.output_tail() << '\n';
      break;
    }
    if (std::chrono::steady_clock::now() >= next_status) {
      std::cerr << "chapter regression progress=" << progress << "s archive=" << find_archive(output_root) << '\n';
      next_status += std::chrono::seconds(30);
    }
  }

  if (crossed_boundary) {
    const fs::path archive = find_archive(output_root);
    if (!archive.empty() && fs::file_size(archive) > 0) {
      // The regression assertion is the lossless chapter transition itself. Terminate immediately rather than
      // conflating it with the independent interactive-stop/EOS-finalization behavior.
      process.StopImmediately();
      fs::remove_all(fixture_root);
      fs::remove_all(output_root);
      return 0;
    }
    std::cerr << "FAIL: pipeline crossed the boundary but did not produce an archive\n";
  }

  process.StopImmediately();
  std::cerr << "Fixture retained at " << fixture_root << " and output at " << output_root << '\n';
  return 1;
}
