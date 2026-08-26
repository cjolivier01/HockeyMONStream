#include "hstream/src/libs/stitching/ScoreboardSelector.h"
#include "hstream/src/libs/stitching/HuginProject.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>

#include <arpa/inet.h>
#include <opencv2/imgcodecs.hpp>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool write_hugin_generation_fixture(const std::filesystem::path& directory) {
  for (const char* name : {
           "hm_project.pto",
           "autooptimiser_out.pto",
           "mapping_0000.tif",
           "mapping_0000_x.tif",
           "mapping_0000_y.tif",
           "mapping_0001.tif",
           "mapping_0001_x.tif",
           "mapping_0001_y.tif",
           "seam_file.png",
       }) {
    std::ofstream output(directory / name, std::ios::binary | std::ios::trunc);
    output << name << '\n';
    if (!output)
      return false;
  }
  return true;
}

bool write_all(int fd, const std::string& value) {
  size_t offset = 0;
  while (offset < value.size()) {
    const ssize_t written = ::send(fd, value.data() + offset, value.size() - offset, MSG_NOSIGNAL);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return false;
    offset += static_cast<size_t>(written);
  }
  return true;
}

std::string http_request(
    unsigned port,
    const std::string& target,
    bool post,
    const std::string& authority_host = "127.0.0.1") {
  const int socket = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (socket < 0)
    return {};
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(socket);
    return {};
  }
  std::ostringstream request;
  request << (post ? "POST " : "GET ") << target << " HTTP/1.1\r\n"
          << "Host: " << authority_host << ':' << port << "\r\n"
          << "Connection: close\r\n";
  if (post) {
    request << "Origin: http://" << authority_host << ':' << port << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: 2\r\n";
  }
  request << "\r\n";
  if (post)
    request << "{}";
  if (!write_all(socket, request.str())) {
    ::close(socket);
    return {};
  }
  ::shutdown(socket, SHUT_WR);
  std::string response;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t count = ::read(socket, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    response.append(buffer.data(), static_cast<size_t>(count));
  }
  ::close(socket);
  return response;
}

bool exercise_http_selector(const std::filesystem::path& directory, bool remote = false) {
  const std::string authority_host = remote ? "scoreboard.test" : "127.0.0.1";
  cv::Mat image(24, 32, CV_8UC3, cv::Scalar(255, 255, 255));
  if (!cv::imwrite((directory / "s.png").string(), image))
    return false;
  int output_pipe[2]{};
  if (::pipe(output_pipe) != 0)
    return false;
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    return false;
  }
  if (child == 0) {
    ::close(output_pipe[0]);
    ::dup2(output_pipe[1], STDERR_FILENO);
    ::close(output_pipe[1]);
    ::unsetenv("HM_NO_SCOREBOARD");
    if (remote) {
      ::setenv("HM_SCOREBOARD_BIND_HOST", "0.0.0.0", 1);
      ::setenv("HM_SCOREBOARD_ALLOW_REMOTE", "1", 1);
      ::setenv("HM_SCOREBOARD_PUBLIC_HOST", authority_host.c_str(), 1);
    } else {
      ::unsetenv("HM_SCOREBOARD_BIND_HOST");
      ::unsetenv("HM_SCOREBOARD_ALLOW_REMOTE");
      ::unsetenv("HM_SCOREBOARD_PUBLIC_HOST");
    }
    ::setenv("HM_SCOREBOARD_CLIENT_TIMEOUT_MS", "250", 1);
    const auto status = hm::stitching::ScoreboardSelector::Run(directory);
    _exit(status.ok() ? 0 : 1);
  }
  ::close(output_pipe[1]);
  std::string startup;
  std::array<char, 512> buffer{};
  std::smatch match;
  const std::regex url_pattern(
      remote ? R"(http://scoreboard[.]test:([0-9]+)/[?]token=([0-9a-f]{64}))"
             : R"(http://127[.]0[.]0[.]1:([0-9]+)/[?]token=([0-9a-f]{64}))");
  while (startup.size() < 4096 && !std::regex_search(startup, match, url_pattern)) {
    const ssize_t count = ::read(output_pipe[0], buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    startup.append(buffer.data(), static_cast<size_t>(count));
  }
  bool ok = std::regex_search(startup, match, url_pattern);
  if (ok) {
    const unsigned port = static_cast<unsigned>(std::stoul(match[1].str()));
    const std::string token = match[2].str();
    int ready_pipe[2]{};
    const bool pipe_created = ::pipe(ready_pipe) == 0;
    ok &= pipe_created;
    pid_t slow_client = -1;
    if (pipe_created)
      slow_client = ::fork();
    ok &= slow_client >= 0;
    if (slow_client == 0) {
      ::close(ready_pipe[0]);
      const int socket = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_port = htons(port);
      ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
      const bool connected =
          socket >= 0 && ::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
      const char ready = connected && ::send(socket, "G", 1, MSG_NOSIGNAL) == 1 ? '1' : '0';
      const ssize_t ready_written = ::write(ready_pipe[1], &ready, 1);
      ::close(ready_pipe[1]);
      ::usleep(1500 * 1000);
      if (socket >= 0)
        ::close(socket);
      _exit(ready == '1' && ready_written == 1 ? 0 : 1);
    }
    if (slow_client > 0) {
      ::close(ready_pipe[1]);
      char ready = '0';
      ok &= expect(::read(ready_pipe[0], &ready, 1) == 1 && ready == '1', "slow scoreboard test client must connect");
      ::close(ready_pipe[0]);
      ::usleep(50 * 1000);
      const auto request_started = std::chrono::steady_clock::now();
      const std::string forbidden_response = http_request(port, "/?token=wrong", false, authority_host);
      ok &= expect(
          forbidden_response.rfind("HTTP/1.1 403 ", 0) == 0,
          "selector must resume with the queued request after a slow client deadline");
      const auto request_elapsed = std::chrono::steady_clock::now() - request_started;
      ok &= expect(
          request_elapsed < std::chrono::seconds(1),
          "a stalled scoreboard client must not block the single-threaded selector");
      int slow_status = 0;
      ok &= expect(
          ::waitpid(slow_client, &slow_status, 0) == slow_client && WIFEXITED(slow_status) &&
              WEXITSTATUS(slow_status) == 0,
          "slow scoreboard test client must exit normally");
    } else if (pipe_created) {
      ::close(ready_pipe[0]);
      ::close(ready_pipe[1]);
    }
    ok &= expect(
        http_request(port, "/?token=" + token, false, authority_host).rfind("HTTP/1.1 200 ", 0) == 0,
        "valid scoreboard selector GET must succeed");
    ok &= expect(
        http_request(port, "/none?token=" + token, true, authority_host).rfind("HTTP/1.1 200 ", 0) == 0,
        "valid scoreboard selector POST must succeed");
  } else {
    ::kill(child, SIGTERM);
  }
  int status = 0;
  ok &= ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  ::close(output_pipe[0]);
  if (!ok)
    std::cerr << "FAIL: native scoreboard HTTP selector did not complete: " << startup << '\n';
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  using Selector = hm::stitching::ScoreboardSelector;
  Selector::Polygon points{{{90, 80}, {10, 10}, {90, 10}, {10, 80}}};
  auto ordered = Selector::ValidateAndOrder(points, 100, 100);
  ok &= expect(ordered.ok(), "valid scoreboard polygon must validate");
  if (ordered.ok()) {
    ok &= expect((*ordered)[0].x == 10 && (*ordered)[0].y == 10, "upper-left must be first");
    ok &= expect((*ordered)[1].x == 90 && (*ordered)[1].y == 10, "upper-right must be second");
    ok &= expect((*ordered)[2].x == 90 && (*ordered)[2].y == 80, "lower-right must be third");
    ok &= expect((*ordered)[3].x == 10 && (*ordered)[3].y == 80, "lower-left must be fourth");
  }
  points[0] = points[1];
  ok &= expect(!Selector::ValidateAndOrder(points, 100, 100).ok(), "duplicate points must fail");
  Selector::Polygon disabled{};
  ok &= expect(Selector::IsDisabled(disabled), "four zero points must be the disabled sentinel");
  ok &= expect(Selector::ValidateAndOrder(disabled, 100, 100).ok(), "disabled sentinel must validate");

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / ("scoreboard-selector-test-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  ok &= expect(write_hugin_generation_fixture(directory), "scoreboard generation fixture must be created");
  auto status = Selector::Save(directory, disabled);
  ok &= expect(status.ok(), "disabled sentinel must save");
  if (status.ok()) {
    const YAML::Node polygon =
        YAML::LoadFile((directory / "config.yaml").string())["rink"]["scoreboard"]["perspective_polygon"];
    ok &= expect(polygon.IsSequence() && polygon.size() == 4, "saved sentinel must contain four points");
    ok &= expect(polygon[3][0].as<int>() == 0 && polygon[3][1].as<int>() == 0, "saved sentinel must stay zero");
  }
  std::filesystem::remove(directory / "config.yaml");
  ok &= expect(exercise_http_selector(directory), "native HTTP selector must enforce its token and save a choice");
  if (std::filesystem::is_regular_file(directory / "config.yaml")) {
    const YAML::Node polygon =
        YAML::LoadFile((directory / "config.yaml").string())["rink"]["scoreboard"]["perspective_polygon"];
    ok &= expect(
        polygon.IsSequence() && polygon.size() == 4 && polygon[0][0].as<int>() == 0,
        "HTTP no-scoreboard choice must persist the disabled sentinel");
  } else {
    ok &= expect(false, "HTTP selector must publish config.yaml");
  }
  std::filesystem::remove(directory / "config.yaml");
  ok &= expect(
      exercise_http_selector(directory, true),
      "remote scoreboard mode must accept its explicit public authority and matching origin");
  auto hugin_lock = hm::stitching::HuginProject::RecoverAndLock(directory);
  ok &= expect(hugin_lock.ok(), "scoreboard generation race test must lock Hugin artifacts");
  std::string stale_generation;
  if (hugin_lock.ok()) {
    const auto generation = hm::stitching::HuginProject::GenerationId(directory, **hugin_lock);
    ok &= expect(generation.ok(), "scoreboard generation race test must identify Hugin artifacts");
    if (generation.ok())
      stale_generation = *generation;
    hugin_lock->reset();
  }
  {
    std::ofstream replacement(directory / "mapping_0000.tif.replacement", std::ios::binary | std::ios::trunc);
    replacement << "replacement mapping\n";
  }
  std::filesystem::rename(directory / "mapping_0000.tif.replacement", directory / "mapping_0000.tif");
  ok &= expect(
      !stale_generation.empty() && absl::IsAborted(Selector::Save(directory, disabled, stale_generation)),
      "a selector opened on a replaced stitched canvas must not publish stale coordinates");
  ::setenv("HM_SCOREBOARD_BIND_HOST", "0.0.0.0", 1);
  ::setenv("HM_SCOREBOARD_ALLOW_REMOTE", "1", 1);
  ::unsetenv("HM_SCOREBOARD_PUBLIC_HOST");
  ok &= expect(
      absl::IsInvalidArgument(Selector::Run(directory)),
      "remote scoreboard mode must require an explicit public host before listening");
  ::unsetenv("HM_SCOREBOARD_BIND_HOST");
  ::unsetenv("HM_SCOREBOARD_ALLOW_REMOTE");
  std::filesystem::remove_all(directory);
  return ok ? 0 : 1;
}
