#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/apps/hstream-cli/PlayerModelCache.h"
#include "yaml-cpp/yaml.h"

namespace {
namespace fs = std::filesystem;
volatile sig_atomic_t cancelled = 0;
void Cancel(int signal) {
  cancelled = signal;
}
void Require(bool valid, const std::string& message) {
  if (!valid)
    throw std::runtime_error(message);
}
bool IsCancelled() {
  static bool eof = false;
  static std::string pending;
  struct pollfd input{STDIN_FILENO, POLLIN, 0};
  if (!eof && ::poll(&input, 1, 0) > 0 && (input.revents & (POLLIN | POLLHUP))) {
    char buffer[4096];
    const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
    Require(count >= 0 || errno == EINTR || errno == EAGAIN, "Cannot read model preparation commands");
    if (count == 0)
      eof = true;
    else if (count > 0) {
      pending.append(buffer, count);
      Require(pending.size() <= 65536, "Model preparation input exceeds the command limit");
    }
    for (size_t end; (end = pending.find('\n')) != std::string::npos;) {
      auto command = pending.substr(0, end);
      pending.erase(0, end + 1);
      if (!command.empty() && command.back() == '\r')
        command.pop_back();
      if (command == "q" || command == "quit")
        cancelled = SIGTERM;
    }
    if (pending == "q" || (eof && pending == "quit")) {
      cancelled = SIGTERM;
      pending.clear();
    }
  }
  return cancelled != 0;
}
void CheckCancellation() {
  Require(!IsCancelled(), "Player model preparation cancelled");
}
bool Enabled(const YAML::Node& node) {
  if (!node || node.IsNull())
    return false;
  return node.IsScalar() && (node.Scalar() == "0" || node.Scalar() == "1") ? node.Scalar() == "1" : node.as<bool>();
}
std::string String(const YAML::Node& node) {
  return !node || node.IsNull() ? "" : node.as<std::string>();
}
void Parent(const std::string& requested) {
  if (requested.empty())
    return;
  Require(requested.find_first_not_of("0123456789") == std::string::npos, "Invalid parent PID");
  const auto expected = std::stoll(requested);
  Require(
      expected > 0 && ::prctl(PR_SET_PDEATHSIG, SIGTERM) == 0 && expected == ::getppid(),
      "Stale or invalid model preparation parent PID");
}
void SyncDirectory(const fs::path& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  Require(descriptor >= 0, "Cannot open model preparation output directory");
  const int result = ::fsync(descriptor);
  ::close(descriptor);
  Require(result == 0, "Cannot flush model preparation output directory");
}
class Publication {
 public:
  ~Publication() {
    for (auto it = owned_.rbegin(); it != owned_.rend(); ++it) {
      struct stat current{};
      if (::lstat(it->path.c_str(), &current) == 0 && current.st_dev == it->device && current.st_ino == it->inode &&
          ::unlink(it->path.c_str()) != 0)
        std::cerr << "Cannot remove incomplete model preparation output: " << it->path << '\n';
    }
  }
  void Write(const fs::path& target, const std::string& text) {
    auto pattern = (target.parent_path() / ".hstream-model-prepare-XXXXXX").string();
    const int descriptor = ::mkstemp(pattern.data());
    Require(descriptor >= 0, "Cannot stage model preparation output");
    struct stat identity{};
    if (::fstat(descriptor, &identity) != 0) {
      ::close(descriptor);
      ::unlink(pattern.c_str());
      throw std::runtime_error("Cannot identify model preparation output");
    }
    owned_.push_back({pattern, identity.st_dev, identity.st_ino});
    size_t position = 0;
    bool good = true;
    while (position < text.size()) {
      const auto count = ::write(descriptor, text.data() + position, text.size() - position);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0) {
        good = false;
        break;
      }
      position += static_cast<size_t>(count);
    }
    good = good && ::fsync(descriptor) == 0;
    ::close(descriptor);
    Require(good, "Cannot write complete model preparation output");
    CheckCancellation();
    Require(
        ::link(pattern.c_str(), target.c_str()) == 0,
        "Cannot publish model preparation output (destination must be new): " + target.string());
    owned_.push_back({target, identity.st_dev, identity.st_ino});
    Require(::unlink(pattern.c_str()) == 0, "Cannot remove model preparation staging file");
    SyncDirectory(target.parent_path());
  }
  void Commit() {
    owned_.clear();
  }

 private:
  struct Owned {
    fs::path path;
    dev_t device;
    ino_t inode;
  };
  std::vector<Owned> owned_;
};
std::string Receipt(const YAML::Node& pipeline, const fs::path& output) {
  YAML::Emitter receipt;
  receipt << YAML::DoubleQuoted << YAML::Flow << YAML::BeginMap << YAML::Key << "version" << YAML::Value << 1
          << YAML::Key << "status" << YAML::Value << "complete" << YAML::Key << "output" << YAML::Value
          << output.string() << YAML::Key << "prepared" << YAML::Value << YAML::BeginSeq;
  auto entry = [&receipt](const std::string& feature, const std::string& model, const fs::path& bundle) {
    Require(fs::is_directory(bundle), "Prepared model bundle is missing: " + bundle.string());
    receipt << YAML::BeginMap << YAML::Key << "feature" << YAML::Value << feature << YAML::Key << "model" << YAML::Value
            << model << YAML::Key << "bundle" << YAML::Value << bundle.string();
    for (const auto& artifact : std::map<std::string, std::string>{
             {"manifest", "manifest.json"},
             {"engine", "model.engine"},
             {"onnx", "model.onnx"},
             {"etlt", "model.etlt"},
             {"tracker_config", "tracker.yaml"}}) {
      const auto path = bundle / artifact.second;
      if (fs::is_regular_file(path))
        receipt << YAML::Key << artifact.first << YAML::Value << path.string();
      else
        Require(artifact.first != "manifest" && artifact.first != "engine", "Prepared model artifact is missing");
    }
    receipt << YAML::EndMap;
  };
  const auto analytics = pipeline["player-analytics"];
  if (analytics && analytics.IsMap()) {
    for (const char* feature : {"pose", "jersey", "action", "team"}) {
      const auto selected = analytics[feature];
      if (selected && Enabled(selected["enable"]) && !String(selected["model"]).empty() &&
          String(selected["model"]) != "custom")
        entry(feature, String(selected["model"]), String(selected["bundle"]));
    }
  }
  const auto tracker = pipeline["tracker"];
  if (tracker && Enabled(tracker["enable"]) && Enabled(tracker["reid-enable"]) &&
      !String(tracker["reid-model"]).empty() && String(tracker["reid-model"]) != "custom")
    entry("reid", String(tracker["reid-model"]), fs::path(String(tracker["reid-config-file"])).parent_path());
  receipt << YAML::EndSeq << YAML::EndMap;
  Require(receipt.good(), "Cannot serialize model preparation receipt");
  return std::string(receipt.c_str()) + "\n";
}
} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout << "hstream-model-prepare --config RESOLVED.yaml --config-directory NATIVE_CONFIGS\n"
                   "  --output NEW_RESOLVED.yaml --receipt NEW_RECEIPT.json [--parent-pid PID]\n";
      return 0;
    }
    std::map<std::string, std::string> arguments;
    for (int index = 1; index < argc; ++index) {
      const std::string name = argv[index];
      Require(
          name == "--config" || name == "--config-directory" || name == "--output" || name == "--receipt" ||
              name == "--parent-pid",
          "Unknown model preparation argument: " + name);
      Require(index + 1 < argc && !arguments.count(name), "Missing or duplicate model preparation argument: " + name);
      arguments[name] = argv[++index];
      Require(!arguments[name].empty(), "Model preparation argument cannot be empty: " + name);
    }
    for (const char* required : {"--config", "--config-directory", "--output", "--receipt"})
      Require(arguments.count(required), std::string("Missing argument: ") + required);
    struct sigaction action{};
    action.sa_handler = Cancel;
    ::sigemptyset(&action.sa_mask);
    Require(
        ::sigaction(SIGTERM, &action, nullptr) == 0 && ::sigaction(SIGINT, &action, nullptr) == 0,
        "Cannot install model preparation cancellation handlers");
    const char* parent = std::getenv("HSTREAM_UI_PARENT_PID");
    Parent(arguments.count("--parent-pid") ? arguments.at("--parent-pid") : parent ? parent : "");
    CheckCancellation();
    const auto config = fs::canonical(arguments.at("--config"));
    const auto directory = fs::canonical(arguments.at("--config-directory"));
    Require(fs::is_directory(directory), "Native model configuration directory is not a directory");
    Require(
        fs::is_regular_file(config) && fs::file_size(config) <= 16 * 1024 * 1024,
        "Resolved model configuration must be a regular file at most 16 MiB");
    const auto output = fs::absolute(arguments.at("--output")).lexically_normal();
    const auto receipt = fs::absolute(arguments.at("--receipt")).lexically_normal();
    Require(
        output != receipt && !fs::exists(output) && !fs::exists(receipt), "Model outputs must be distinct new files");
    Require(
        fs::is_directory(output.parent_path()) && fs::is_directory(receipt.parent_path()),
        "Model output parent directories must exist");
    auto document = YAML::LoadFile(config.string());
    Require(document.IsMap(), "Resolved model configuration must be a mapping");
    if (!document["pipeline"].IsDefined()) {
      YAML::Node wrapped;
      wrapped["pipeline"] = document;
      document = wrapped;
    }
    auto pipeline = document["pipeline"];
    Require(pipeline.IsMap(), "Resolved pipeline must be a mapping");
    hm::pipeline::PlayerModelCacheOptions options;
    options.cancelled = IsCancelled;
    options.progress = [](std::string message) {
      std::cout << "HSTREAM_STARTUP stage=models message=" << message << std::endl;
    };
    const auto status = hm::pipeline::PreparePlayerModelCache(pipeline, directory, options);
    Require(status.ok(), status.ToString());
    CheckCancellation();
    YAML::Emitter resolved;
    resolved << document;
    Require(resolved.good(), "Cannot serialize resolved model configuration");
    const auto result = Receipt(pipeline, output);
    Publication publication;
    publication.Write(output, std::string(resolved.c_str()) + "\n");
    publication.Write(receipt, result);
    CheckCancellation();
    publication.Commit();
    std::cout << "HSTREAM_MODEL_PREPARATION " << result << std::flush;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "hstream-model-prepare: " << error.what() << '\n';
    return cancelled ? 128 + cancelled : 1;
  }
}
