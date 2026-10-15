#include "hstream/src/libs/publication/Publication.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool success, const char* message) {
  if (!success)
    throw std::runtime_error(message);
}

std::string execute(const std::vector<std::string>& arguments, const std::string& input = {}) {
  int input_pipe[2], output_pipe[2];
  require(pipe(input_pipe) == 0 && pipe(output_pipe) == 0, "create child pipes");
  require(write(input_pipe[1], input.data(), input.size()) == static_cast<ssize_t>(input.size()), "queue child input");
  close(input_pipe[1]);
  std::vector<char*> argv;
  for (const auto& argument : arguments)
    argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  const pid_t child = fork();
  require(child >= 0, "fork child");
  if (child == 0) {
    if (dup2(input_pipe[0], STDIN_FILENO) < 0 || dup2(output_pipe[1], STDOUT_FILENO) < 0)
      _exit(126);
    close(input_pipe[0]);
    close(output_pipe[0]);
    close(output_pipe[1]);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(input_pipe[0]);
  close(output_pipe[1]);
  std::string output;
  char buffer[1024];
  while (true) {
    const ssize_t count = read(output_pipe[0], buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR)
      continue;
    require(count >= 0, "read child output");
    if (count == 0)
      break;
    output.append(buffer, count);
  }
  close(output_pipe[0]);
  int status;
  while (waitpid(child, &status, 0) < 0)
    require(errno == EINTR, "wait for child");
  require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child must exit successfully");
  return output;
}
} // namespace

int main() {
  try {
    namespace fs = std::filesystem;
    const fs::path directory = fs::path(std::getenv("TEST_TMPDIR") ? std::getenv("TEST_TMPDIR") : "/tmp") /
        ("remux-stdin-test-" + std::to_string(getpid()));
    fs::create_directories(directory);
    const auto source = directory / "source.mkv";
    const auto destination = directory / "published.mp4";
    execute(
        {"ffmpeg",
         "-hide_banner",
         "-loglevel",
         "error",
         "-f",
         "lavfi",
         "-i",
         "color=size=64x64:rate=5",
         "-t",
         "2",
         "-c:v",
         "libx264",
         source.string()});
    auto arguments = hm::publication::RemuxArguments(source.string(), destination.string(), false);
    // Pace this tiny fixture so ffmpeg polls stdin while input remains. Without
    // -nostdin, queued q ends the remux early but still reports exit status zero.
    arguments.insert(std::find(arguments.begin(), arguments.end(), "-i"), "-re");
    arguments.insert(arguments.begin(), "ffmpeg");
    execute(arguments, "q\n");
    const auto frames = execute(
        {"ffprobe",
         "-v",
         "error",
         "-select_streams",
         "v:0",
         "-count_packets",
         "-show_entries",
         "stream=nb_read_packets",
         "-of",
         "csv=p=0",
         destination.string()});
    require(frames == "10\n", "queued input must not shorten the published video");
    fs::remove_all(directory);
    std::cout << "Remux stdin test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
