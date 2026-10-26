#include "hstream/src/libs/player_analytics/PreparedModelValidation.h"

#include <fcntl.h>
#include <glib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace pa = hm::player_analytics;
namespace {
volatile sig_atomic_t interrupted = 0;
void stop(int signal) {
  interrupted = signal;
}
void require(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string text(const YAML::Node& node, const char* key) {
  const auto value = node[key];
  require(value.IsScalar(), std::string("Expected prepared model field ") + key);
  const auto result = value.as<std::string>();
  require(
      !result.empty() && result.size() <= 1024 && result.find('\0') == std::string::npos,
      std::string("Invalid prepared model field ") + key);
  return result;
}
fs::path confined(const fs::path& root, const std::string& relative) {
  const fs::path input(relative);
  require(!input.is_absolute(), "Prepared model paths must be relative");
  auto path = root;
  for (const auto& component : input) {
    require(component != "." && component != ".." && !component.empty(), "Invalid prepared model path component");
    path /= component;
    require(!fs::is_symlink(fs::symlink_status(path)), "Prepared model inputs cannot contain symbolic links");
  }
  return path;
}
YAML::Node bounded_document(const fs::path& path) {
  require(
      fs::is_regular_file(path) && fs::file_size(path) > 0 && fs::file_size(path) <= 1024 * 1024,
      "Prepared model request exceeds its metadata bound");
  std::ifstream source(path, std::ios::binary);
  std::string data(1024 * 1024 + 1, '\0');
  source.read(data.data(), data.size());
  const auto size = source.gcount();
  require(size > 0 && size <= 1024 * 1024 && !source.bad(), "Cannot read bounded prepared model request");
  data.resize(size);
  return YAML::Load(data);
}
bool same(const struct stat& left, const struct stat& right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino && left.st_size == right.st_size &&
      left.st_mtim.tv_sec == right.st_mtim.tv_sec && left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
      left.st_ctim.tv_sec == right.st_ctim.tv_sec && left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}
std::string digest(const fs::path& path, std::uint64_t size) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  require(fd >= 0, "Cannot pin prepared model input");
  const auto close_fd = [](int* value) {
    ::close(*value);
    delete value;
  };
  std::unique_ptr<int, decltype(close_fd)> owned(new int(fd), close_fd);
  struct stat before{}, after{};
  require(
      ::fstat(fd, &before) == 0 && S_ISREG(before.st_mode) && before.st_size > 0 &&
          std::uint64_t(before.st_size) == size,
      "Prepared model file identity does not match its admitted size");
  std::unique_ptr<GChecksum, decltype(&g_checksum_free)> checksum(g_checksum_new(G_CHECKSUM_SHA256), g_checksum_free);
  std::array<unsigned char, 1024 * 1024> buffer;
  std::uint64_t total = 0;
  while (true) {
    require(!interrupted, "cancelled");
    const auto count = ::read(fd, buffer.data(), buffer.size());
    require(count >= 0, "Cannot read prepared model input");
    if (!count)
      break;
    total += count;
    require(total <= size, "Prepared model input grew while reading");
    g_checksum_update(checksum.get(), buffer.data(), count);
  }
  require(
      ::fstat(fd, &after) == 0 && same(before, after) && total == size, "Prepared model input changed while reading");
  return g_checksum_get_string(checksum.get());
}
void publish(const fs::path& destination, const YAML::Node& result) {
  const auto temporary = destination.string() + ".part";
  const auto body = YAML::Dump(result);
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  require(fd >= 0, "Cannot create prepared model validation receipt");
  const auto wrote = ::write(fd, body.data(), body.size());
  const bool complete = wrote == ssize_t(body.size()) && ::fsync(fd) == 0;
  ::close(fd);
  require(complete, "Cannot synchronize prepared model validation receipt");
  require(::link(temporary.c_str(), destination.c_str()) == 0, "Prepared model validation receipt already exists");
  require(::unlink(temporary.c_str()) == 0, "Cannot retire temporary model receipt");
  const int directory = ::open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  require(directory >= 0, "Cannot pin model validation directory");
  const bool synced = ::fsync(directory) == 0;
  ::close(directory);
  require(synced, "Cannot synchronize model validation directory");
}
} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "hstream-prepared-model-validate --request /private/model/model-request.json\n"
                 "prepared-model-validation-v1; CPU-only shared Qt manifest and file checks.\n";
    return 0;
  }
  if (argc != 3 || std::string(argv[1]) != "--request")
    return 64;
  try {
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    const auto request_path = fs::canonical(argv[2]);
    const auto request = bounded_document(request_path);
    const auto root = fs::canonical(text(request, "root"));
    require(
        root == request_path.parent_path() && request["schema"].as<int>() == 1,
        "Prepared model validation request has the wrong owner");
    const auto id = text(request, "id");
    const auto feature = text(request, "feature");
    require(
        feature == "pose" || feature == "jersey" || feature == "action" || feature == "team" || feature == "reid",
        "Unsupported prepared model feature");
    const auto bundle = confined(root, text(request, "bundle"));
    require(fs::is_directory(bundle), "Prepared model bundle must be a directory");
    const auto inputs = request["files"];
    require(inputs.IsSequence() && inputs.size() > 0 && inputs.size() <= 64, "Invalid bounded prepared model file set");
    std::set<fs::path> expected;
    for (const auto& input : inputs) {
      const auto path = confined(bundle, text(input, "path"));
      require(expected.insert(path).second, "Prepared model inputs repeat a filename");
      require(
          digest(path, input["size"].as<std::uint64_t>()) == text(input, "sha256"),
          "Prepared model file checksum changed after import");
    }
    for (const auto& item : fs::recursive_directory_iterator(bundle)) {
      require(!item.is_symlink(), "Prepared model inputs cannot contain symbolic links");
      if (!item.is_directory())
        require(
            item.is_regular_file() && expected.erase(item.path()) == 1,
            "Prepared model bundle contains unadmitted files");
    }
    require(expected.empty(), "Prepared model bundle is incomplete");
    YAML::Node result;
    result["schema"] = 1;
    result["id"] = id;
    result["state"] = "complete";
    result["feature"] = feature;
    result["files"] = inputs;
    if (feature == "reid") {
      const auto entry = text(request, "entry");
      const auto config = confined(bundle, entry);
      const auto checked = pa::ValidatePreparedReidConfig(config);
      require(checked.ok(), checked.status().ToString());
      const auto relative = checked->lexically_relative(bundle);
      require(
          !relative.empty() && !relative.is_absolute() && *relative.begin() != "..",
          "Appearance engine escapes its bundle");
      confined(bundle, relative.generic_string());
      result["entry"] = entry;
      result["engine"] = relative.generic_string();
      result["maximum_batch"] = 0;
      result["topology"] = "";
    } else {
      const auto kind = feature == "pose" ? pa::ModelFeature::kPose
          : feature == "jersey"           ? pa::ModelFeature::kJersey
          : feature == "action"           ? pa::ModelFeature::kAction
                                          : pa::ModelFeature::kTeam;
      const auto checked = pa::ValidatePreparedModelBundle(bundle, kind, 1);
      require(checked.ok(), checked.status().ToString());
      result["entry"] = "manifest.json";
      result["model_id"] = checked->model_id;
      result["maximum_batch"] = checked->maximum_batch;
      result["topology"] = checked->topology_id;
    }
    require(!interrupted, "cancelled");
    publish(root / "model-validation.yaml", result);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "prepared model validation: " << error.what() << '\n';
    return 1;
  }
}
