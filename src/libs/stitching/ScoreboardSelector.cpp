#include "hstream/src/libs/stitching/ScoreboardSelector.h"
#include "hstream/src/libs/stitching/GameConfig.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <opencv2/imgcodecs.hpp>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include "absl/status/status.h"

namespace hm::stitching {
namespace {

namespace fs = std::filesystem;

constexpr size_t kMaximumHeaderBytes = 16 * 1024;
constexpr size_t kMaximumBodyBytes = 4 * 1024;
constexpr auto kTokenLifetime = std::chrono::minutes(30);
constexpr auto kDefaultClientLifetime = std::chrono::seconds(10);

std::chrono::milliseconds client_lifetime() {
  const char* configured = std::getenv("HM_SCOREBOARD_CLIENT_TIMEOUT_MS");
  if (configured == nullptr || *configured == '\0')
    return kDefaultClientLifetime;
  try {
    const long long milliseconds = std::stoll(configured);
    if (milliseconds >= 100 && milliseconds <= 60000)
      return std::chrono::milliseconds(milliseconds);
  } catch (const std::exception&) {
  }
  std::cerr << "Warning: ignoring invalid HM_SCOREBOARD_CLIENT_TIMEOUT_MS=" << configured << '\n';
  return kDefaultClientLifetime;
}

absl::Status set_socket_deadline(int socket, int option, std::chrono::steady_clock::time_point deadline) {
  const auto remaining = deadline - std::chrono::steady_clock::now();
  if (remaining <= std::chrono::steady_clock::duration::zero())
    return absl::DeadlineExceededError("Scoreboard HTTP client deadline expired");
  const auto microseconds =
      std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::microseconds>(remaining).count());
  const timeval timeout{
      static_cast<time_t>(microseconds / 1000000),
      static_cast<suseconds_t>(microseconds % 1000000),
  };
  if (::setsockopt(socket, SOL_SOCKET, option, &timeout, sizeof(timeout)) != 0)
    return absl::InternalError("Unable to apply scoreboard HTTP client deadline");
  return absl::OkStatus();
}

std::string random_token() {
  std::array<unsigned char, 32> bytes{};
  std::random_device source;
  for (unsigned char& byte : bytes)
    byte = static_cast<unsigned char>(source());
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned char byte : bytes)
    output << std::setw(2) << static_cast<unsigned>(byte);
  return output.str();
}

absl::Status write_all(int socket, const void* data, size_t size, std::chrono::steady_clock::time_point deadline) {
  const char* cursor = static_cast<const char*>(data);
  while (size > 0) {
    auto timeout_status = set_socket_deadline(socket, SO_SNDTIMEO, deadline);
    if (!timeout_status.ok())
      return timeout_status;
    const ssize_t written = ::send(socket, cursor, size, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return absl::DeadlineExceededError("Scoreboard HTTP response deadline expired");
      return absl::InternalError("Failed writing scoreboard HTTP response");
    }
    cursor += written;
    size -= static_cast<size_t>(written);
  }
  return absl::OkStatus();
}

absl::Status respond(
    int socket,
    int status,
    const char* reason,
    const std::string& content_type,
    const std::string& body,
    std::chrono::steady_clock::time_point deadline) {
  std::ostringstream header;
  header << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "X-Content-Type-Options: nosniff\r\n"
         << "Content-Security-Policy: default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
            "img-src 'self' data:; connect-src 'self'\r\n"
         << "Connection: close\r\n\r\n";
  const std::string header_bytes = header.str();
  auto result = write_all(socket, header_bytes.data(), header_bytes.size(), deadline);
  if (!result.ok())
    return result;
  return write_all(socket, body.data(), body.size(), deadline);
}

void respond_best_effort(
    int socket,
    int status,
    const char* reason,
    const std::string& content_type,
    const std::string& body,
    std::chrono::steady_clock::time_point deadline) {
  const auto result = respond(socket, status, reason, content_type, body, deadline);
  if (!result.ok())
    std::cerr << "Warning: unable to send scoreboard selector response: " << result << '\n';
}

struct Request {
  std::string method;
  std::string target;
  std::map<std::string, std::string> headers;
  std::string body;
};

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  return value;
}

absl::StatusOr<Request> read_request(int socket, std::chrono::steady_clock::time_point deadline) {
  std::string bytes;
  std::array<char, 2048> buffer{};
  size_t end = std::string::npos;
  while ((end = bytes.find("\r\n\r\n")) == std::string::npos) {
    auto timeout_status = set_socket_deadline(socket, SO_RCVTIMEO, deadline);
    if (!timeout_status.ok())
      return timeout_status;
    const ssize_t count = ::recv(socket, buffer.data(), buffer.size(), 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return absl::DeadlineExceededError("Scoreboard HTTP request deadline expired");
    if (count <= 0)
      return absl::InvalidArgumentError("Incomplete scoreboard HTTP request");
    bytes.append(buffer.data(), static_cast<size_t>(count));
    if (bytes.size() > kMaximumHeaderBytes)
      return absl::ResourceExhaustedError("HTTP headers are too large");
  }
  const std::string header_block = bytes.substr(0, end);
  std::istringstream lines(header_block);
  std::string line;
  Request request;
  if (!std::getline(lines, line))
    return absl::InvalidArgumentError("Missing HTTP request line");
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  std::istringstream request_line(line);
  std::string version;
  if (!(request_line >> request.method >> request.target >> version) || version != "HTTP/1.1") {
    return absl::InvalidArgumentError("Malformed HTTP request line");
  }
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos)
      return absl::InvalidArgumentError("Malformed HTTP header");
    std::string value = line.substr(colon + 1);
    value.erase(0, value.find_first_not_of(" \t"));
    request.headers[lowercase(line.substr(0, colon))] = value;
  }
  size_t content_length = 0;
  if (auto found = request.headers.find("content-length"); found != request.headers.end()) {
    try {
      content_length = std::stoull(found->second);
    } catch (const std::exception&) {
      return absl::InvalidArgumentError("Invalid Content-Length");
    }
  }
  if (content_length > kMaximumBodyBytes)
    return absl::ResourceExhaustedError("HTTP request body is too large");
  request.body = bytes.substr(end + 4);
  while (request.body.size() < content_length) {
    auto timeout_status = set_socket_deadline(socket, SO_RCVTIMEO, deadline);
    if (!timeout_status.ok())
      return timeout_status;
    const ssize_t count =
        ::recv(socket, buffer.data(), std::min(buffer.size(), content_length - request.body.size()), 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return absl::DeadlineExceededError("Scoreboard HTTP request deadline expired");
    if (count <= 0)
      return absl::InvalidArgumentError("Incomplete HTTP request body");
    request.body.append(buffer.data(), static_cast<size_t>(count));
  }
  if (request.body.size() > content_length)
    request.body.resize(content_length);
  return request;
}

bool target_has_token(const std::string& target, const std::string& token) {
  const size_t query = target.find('?');
  if (query == std::string::npos)
    return false;
  std::istringstream fields(target.substr(query + 1));
  std::string field;
  while (std::getline(fields, field, '&')) {
    if (field == "token=" + token)
      return true;
  }
  return false;
}

std::string target_path(const std::string& target) {
  return target.substr(0, target.find('?'));
}

absl::StatusOr<ScoreboardSelector::Polygon> parse_polygon(const std::string& json) {
  try {
    const YAML::Node root = YAML::Load(json);
    const YAML::Node points = root["points"];
    if (!root.IsMap() || !points || !points.IsSequence() || points.size() != 4) {
      return absl::InvalidArgumentError("Save request must contain exactly four points");
    }
    ScoreboardSelector::Polygon polygon;
    for (size_t index = 0; index < polygon.size(); ++index) {
      if (!points[index].IsSequence() || points[index].size() != 2) {
        return absl::InvalidArgumentError("Every scoreboard point must contain x and y");
      }
      polygon[index] = {points[index][0].as<int>(), points[index][1].as<int>()};
    }
    return polygon;
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Invalid scoreboard JSON: " + std::string(exception.what()));
  }
}

std::string selector_html(int width, int height, const std::optional<ScoreboardSelector::Polygon>& existing) {
  std::ostringstream initial;
  initial << '[';
  if (existing.has_value() && !ScoreboardSelector::IsDisabled(*existing)) {
    for (size_t i = 0; i < existing->size(); ++i) {
      if (i)
        initial << ',';
      initial << '[' << (*existing)[i].x << ',' << (*existing)[i].y << ']';
    }
  }
  initial << ']';
  std::ostringstream html;
  html << R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>HMStream scoreboard selector</title><style>body{font-family:sans-serif;background:#111;color:#eee;margin:1rem}#wrap{position:relative;display:inline-block;max-width:100%}img{display:block;max-width:100%;height:auto}canvas{position:absolute;inset:0;width:100%;height:100%;cursor:crosshair}button{margin:.75rem .5rem 0 0;padding:.6rem 1rem}</style></head><body>
<h1>Select scoreboard corners</h1><p>Click upper-left, upper-right, lower-right, and lower-left. You can also explicitly disable the scoreboard.</p><div id="wrap"><img id="image"><canvas id="overlay"></canvas></div><br><button id="save">Save</button><button id="clear">Clear</button><button id="none">No scoreboard</button><button id="cancel">Cancel</button><pre id="status"></pre><script>
const token=new URLSearchParams(location.search).get('token');const image=document.getElementById('image'),canvas=document.getElementById('overlay'),ctx=canvas.getContext('2d');let points=)HTML"
       << initial.str() << ";const nativeWidth=" << width << ",nativeHeight=" << height << R"HTML(;
image.src='/image?token='+encodeURIComponent(token);image.onload=()=>{canvas.width=nativeWidth;canvas.height=nativeHeight;draw()};function draw(){ctx.clearRect(0,0,canvas.width,canvas.height);ctx.fillStyle='#ff2d55';ctx.strokeStyle='#ff2d55';ctx.lineWidth=5;points.forEach((p,i)=>{ctx.beginPath();ctx.arc(p[0],p[1],12,0,Math.PI*2);ctx.fill();ctx.fillText(String(i+1),p[0]+15,p[1]-15)});if(points.length>1){ctx.beginPath();ctx.moveTo(...points[0]);points.slice(1).forEach(p=>ctx.lineTo(...p));ctx.stroke()}}canvas.onclick=e=>{if(points.length>=4)return;const r=canvas.getBoundingClientRect();points.push([Math.round((e.clientX-r.left)*nativeWidth/r.width),Math.round((e.clientY-r.top)*nativeHeight/r.height)]);draw()};
async function post(path,body='{}'){const r=await fetch(path+'?token='+encodeURIComponent(token),{method:'POST',headers:{'Content-Type':'application/json'},body});document.getElementById('status').textContent=await r.text();if(r.ok)document.querySelectorAll('button').forEach(b=>b.disabled=true)}document.getElementById('save').onclick=()=>post('/save',JSON.stringify({points}));document.getElementById('clear').onclick=()=>{points=[];draw()};document.getElementById('none').onclick=()=>post('/none');document.getElementById('cancel').onclick=()=>post('/cancel');</script></body></html>)HTML";
  return html.str();
}

std::optional<ScoreboardSelector::Polygon> load_existing(const fs::path& game_dir) {
  try {
    auto loaded_config = load_game_config_file(game_dir / "config.yaml");
    if (!loaded_config.ok() || !loaded_config->has_value())
      return std::nullopt;
    const YAML::Node polygon = (**loaded_config)["rink"]["scoreboard"]["perspective_polygon"];
    if (!polygon || !polygon.IsSequence() || polygon.size() != 4)
      return std::nullopt;
    ScoreboardSelector::Polygon result;
    for (size_t index = 0; index < result.size(); ++index) {
      if (!polygon[index].IsSequence() || polygon[index].size() != 2)
        return std::nullopt;
      result[index] = {polygon[index][0].as<int>(), polygon[index][1].as<int>()};
    }
    return result;
  } catch (...) {
    return std::nullopt;
  }
}

bool valid_public_host(const std::string& host) {
  if (host.empty() || host.size() > 253 || host == "0.0.0.0" || host == "127.0.0.1" || host == "localhost" ||
      host.front() == '.' || host.back() == '.') {
    return false;
  }
  size_t label_length = 0;
  bool label_starts_with_hyphen = false;
  char previous = '\0';
  for (unsigned char character : host) {
    if (character == '.') {
      if (label_length == 0 || label_length > 63 || label_starts_with_hyphen || previous == '-')
        return false;
      label_length = 0;
      label_starts_with_hyphen = false;
    } else if (std::isalnum(character) || character == '-') {
      if (label_length == 0)
        label_starts_with_hyphen = character == '-';
      ++label_length;
    } else {
      return false;
    }
    previous = static_cast<char>(character);
  }
  return label_length > 0 && label_length <= 63 && !label_starts_with_hyphen && previous != '-';
}

} // namespace

bool ScoreboardSelector::IsDisabled(const Polygon& polygon) {
  return std::all_of(
      polygon.begin(), polygon.end(), [](const ScoreboardPoint& point) { return point.x == 0 && point.y == 0; });
}

absl::StatusOr<ScoreboardSelector::Polygon> ScoreboardSelector::ValidateAndOrder(
    Polygon polygon,
    int image_width,
    int image_height) {
  if (image_width <= 0 || image_height <= 0)
    return absl::InvalidArgumentError("Invalid scoreboard image dimensions");
  if (IsDisabled(polygon))
    return polygon;
  for (const ScoreboardPoint& point : polygon) {
    if (point.x < 0 || point.y < 0 || point.x >= image_width || point.y >= image_height) {
      return absl::OutOfRangeError("Scoreboard point lies outside the stitched image");
    }
  }
  Polygon ordered;
  ordered[0] = *std::min_element(
      polygon.begin(), polygon.end(), [](const auto& a, const auto& b) { return a.x + a.y < b.x + b.y; });
  ordered[2] = *std::max_element(
      polygon.begin(), polygon.end(), [](const auto& a, const auto& b) { return a.x + a.y < b.x + b.y; });
  ordered[1] = *std::max_element(
      polygon.begin(), polygon.end(), [](const auto& a, const auto& b) { return a.x - a.y < b.x - b.y; });
  ordered[3] = *std::min_element(
      polygon.begin(), polygon.end(), [](const auto& a, const auto& b) { return a.x - a.y < b.x - b.y; });
  std::array<std::pair<int, int>, 4> unique;
  for (size_t index = 0; index < ordered.size(); ++index)
    unique[index] = {ordered[index].x, ordered[index].y};
  std::sort(unique.begin(), unique.end());
  if (std::adjacent_find(unique.begin(), unique.end()) != unique.end()) {
    return absl::InvalidArgumentError("Scoreboard points must be distinct");
  }
  int64_t twice_area = 0;
  for (size_t index = 0; index < ordered.size(); ++index) {
    const auto& a = ordered[index];
    const auto& b = ordered[(index + 1) % ordered.size()];
    twice_area += static_cast<int64_t>(a.x) * b.y - static_cast<int64_t>(a.y) * b.x;
  }
  if (std::abs(twice_area) < 2)
    return absl::InvalidArgumentError("Scoreboard polygon has zero area");
  return ordered;
}

absl::Status ScoreboardSelector::Save(const fs::path& game_dir, const Polygon& polygon) {
  auto config_lock = GameConfigTransactionLock::Acquire(game_dir);
  if (!config_lock.ok())
    return config_lock.status();
  const fs::path config_path = game_dir / "config.yaml";
  YAML::Node config(YAML::NodeType::Map);
  try {
    if (fs::is_regular_file(config_path))
      config = YAML::LoadFile(config_path.string());
    YAML::Node points(YAML::NodeType::Sequence);
    for (const ScoreboardPoint& point : polygon) {
      YAML::Node pair(YAML::NodeType::Sequence);
      pair.push_back(point.x);
      pair.push_back(point.y);
      points.push_back(pair);
    }
    config["rink"]["scoreboard"]["perspective_polygon"] = points;
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to update scoreboard config: " + std::string(exception.what()));
  }
  std::ostringstream serialized;
  serialized << config << '\n';
  return publish_game_config(game_dir, serialized.str());
}

absl::Status ScoreboardSelector::Run(const fs::path& game_dir) {
  if (const char* disabled = std::getenv("HM_NO_SCOREBOARD"); disabled != nullptr && std::string(disabled) == "1") {
    return Save(game_dir, {});
  }
  const fs::path image_path = game_dir / "s.png";
  const cv::Mat dimensions = cv::imread(image_path.string(), cv::IMREAD_GRAYSCALE);
  if (dimensions.empty())
    return absl::NotFoundError("Scoreboard selector requires " + image_path.string());
  std::ifstream image_input(image_path, std::ios::binary);
  const std::string image_bytes((std::istreambuf_iterator<char>(image_input)), std::istreambuf_iterator<char>());
  if (image_bytes.empty())
    return absl::FailedPreconditionError("Scoreboard selector image is empty");

  std::string bind_host = "127.0.0.1";
  if (const char* configured = std::getenv("HM_SCOREBOARD_BIND_HOST"); configured != nullptr && *configured != '\0') {
    bind_host = configured;
  }
  if (bind_host != "127.0.0.1" && bind_host != "0.0.0.0") {
    return absl::InvalidArgumentError("HM_SCOREBOARD_BIND_HOST must be 127.0.0.1 or 0.0.0.0");
  }
  if (bind_host != "127.0.0.1") {
    const char* allow = std::getenv("HM_SCOREBOARD_ALLOW_REMOTE");
    if (allow == nullptr || std::string(allow) != "1") {
      return absl::PermissionDeniedError("Non-loopback scoreboard binding requires HM_SCOREBOARD_ALLOW_REMOTE=1");
    }
  }

  std::string browser_host = "127.0.0.1";
  if (bind_host == "0.0.0.0") {
    const char* configured_public_host = std::getenv("HM_SCOREBOARD_PUBLIC_HOST");
    if (configured_public_host == nullptr || !valid_public_host(configured_public_host)) {
      return absl::InvalidArgumentError(
          "Remote scoreboard binding requires a valid, non-loopback HM_SCOREBOARD_PUBLIC_HOST hostname or IPv4 address");
    }
    browser_host = configured_public_host;
  }

  const int server = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (server < 0)
    return absl::InternalError("Unable to create scoreboard selector socket");
  struct SocketCleanup {
    int fd;
    ~SocketCleanup() {
      ::close(fd);
    }
  } cleanup{server};
  int reuse = 1;
  ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = 0;
  if (::inet_pton(AF_INET, bind_host.c_str(), &address.sin_addr) != 1 ||
      ::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(server, 4) != 0) {
    return absl::InternalError("Unable to bind scoreboard selector to " + bind_host);
  }
  socklen_t address_size = sizeof(address);
  if (::getsockname(server, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
    return absl::InternalError("Unable to read scoreboard selector port");
  }
  const unsigned port = ntohs(address.sin_port);
  const std::string token = random_token();
  const std::string expected_host = browser_host + ":" + std::to_string(port);
  const std::string expected_origin = "http://" + expected_host;
  std::cerr << "Scoreboard corners are not configured. Open this private, expiring URL:\n  " << expected_origin
            << "/?token=" << token << "\n";
  std::cerr << std::flush;

  const auto deadline = std::chrono::steady_clock::now() + kTokenLifetime;
  while (std::chrono::steady_clock::now() < deadline) {
    fd_set reads;
    FD_ZERO(&reads);
    FD_SET(server, &reads);
    timeval timeout{1, 0};
    const int selected = ::select(server + 1, &reads, nullptr, nullptr, &timeout);
    if (selected < 0) {
      if (errno == EINTR)
        continue;
      return absl::InternalError("Scoreboard selector wait failed");
    }
    if (selected == 0)
      continue;
    const int client = ::accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      if (errno == EINTR)
        continue;
      return absl::InternalError("Scoreboard selector accept failed");
    }
    struct ClientCleanup {
      int fd;
      ~ClientCleanup() {
        ::close(fd);
      }
    } client_cleanup{client};
    const auto client_deadline = std::min(deadline, std::chrono::steady_clock::now() + client_lifetime());
    auto request = read_request(client, client_deadline);
    if (!request.ok()) {
      respond_best_effort(
          client,
          400,
          "Bad Request",
          "text/plain; charset=utf-8",
          std::string(request.status().message()),
          client_deadline);
      continue;
    }
    const auto host = request->headers.find("host");
    const bool valid_host = host != request->headers.end() &&
        (host->second == expected_host ||
         (bind_host == "127.0.0.1" && host->second == "localhost:" + std::to_string(port)));
    if (!valid_host || !target_has_token(request->target, token)) {
      respond_best_effort(
          client, 403, "Forbidden", "text/plain; charset=utf-8", "Invalid selector capability\n", client_deadline);
      continue;
    }
    const std::string path = target_path(request->target);
    if (request->method == "GET" && path == "/") {
      respond_best_effort(
          client,
          200,
          "OK",
          "text/html; charset=utf-8",
          selector_html(dimensions.cols, dimensions.rows, load_existing(game_dir)),
          client_deadline);
      continue;
    }
    if (request->method == "GET" && path == "/image") {
      respond_best_effort(client, 200, "OK", "image/png", image_bytes, client_deadline);
      continue;
    }
    const auto origin = request->headers.find("origin");
    if (request->method != "POST" || origin == request->headers.end() ||
        (origin->second != expected_origin &&
         (bind_host != "127.0.0.1" || origin->second != "http://localhost:" + std::to_string(port)))) {
      respond_best_effort(
          client, 403, "Forbidden", "text/plain; charset=utf-8", "Invalid mutation origin\n", client_deadline);
      continue;
    }
    if (path == "/cancel") {
      respond_best_effort(client, 200, "OK", "text/plain; charset=utf-8", "Selection cancelled\n", client_deadline);
      return absl::CancelledError("Scoreboard selection was cancelled");
    }
    Polygon polygon{};
    if (path == "/save") {
      auto parsed = parse_polygon(request->body);
      if (!parsed.ok()) {
        respond_best_effort(
            client,
            400,
            "Bad Request",
            "text/plain; charset=utf-8",
            std::string(parsed.status().message()),
            client_deadline);
        continue;
      }
      auto ordered = ValidateAndOrder(*parsed, dimensions.cols, dimensions.rows);
      if (!ordered.ok()) {
        respond_best_effort(
            client,
            400,
            "Bad Request",
            "text/plain; charset=utf-8",
            std::string(ordered.status().message()),
            client_deadline);
        continue;
      }
      polygon = *ordered;
    } else if (path != "/none") {
      respond_best_effort(
          client, 404, "Not Found", "text/plain; charset=utf-8", "Unknown selector endpoint\n", client_deadline);
      continue;
    }
    auto saved = Save(game_dir, polygon);
    if (!saved.ok()) {
      respond_best_effort(
          client,
          500,
          "Internal Server Error",
          "text/plain; charset=utf-8",
          std::string(saved.message()),
          client_deadline);
      return saved;
    }
    respond_best_effort(
        client, 200, "OK", "text/plain; charset=utf-8", "Scoreboard configuration saved\n", client_deadline);
    return absl::OkStatus();
  }
  return absl::DeadlineExceededError("Scoreboard selector capability expired");
}

} // namespace hm::stitching
