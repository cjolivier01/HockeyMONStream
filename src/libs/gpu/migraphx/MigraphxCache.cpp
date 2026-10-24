#include "hstream/src/libs/gpu/migraphx/MigraphxCache.h"

#include <fcntl.h>
#include <link.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "hstream/src/libs/assets/AssetManager.h"
#include "hstream/src/libs/gpu/Runtime.h"

extern char** environ;

namespace hm::gpu {
namespace {
namespace fs = std::filesystem;
void Require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error("MIGraphX cache: " + message);
}
struct Fd {
  int value;
  explicit Fd(int value) : value(value) {}
  ~Fd() {
    if (value >= 0)
      close(value);
  }
  fs::path path() const {
    return "/proc/self/fd/" + std::to_string(value);
  }
};
std::string Digest(const fs::path& path) {
  auto hash = assets::AssetManager::Sha256(path);
  Require(hash.ok(), hash.status().ToString());
  return *hash;
}
std::string DigestBytes(const std::string& bytes) {
  auto hash = assets::AssetManager::Sha256Bytes(bytes);
  Require(hash.ok(), hash.status().ToString());
  return *hash;
}
void PrivateDirectory(const fs::path& path) {
  Fd fd(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  struct stat info {};
  Require(
      fd.value >= 0 && fstat(fd.value, &info) == 0 && info.st_uid == geteuid() && (info.st_mode & 077) == 0,
      "directory must be private, owned by this user and not a symlink: " + path.string());
}
void BoundedFile(int fd, size_t maximum) {
  struct stat info {};
  Require(
      fd >= 0 && fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 &&
          uint64_t(info.st_size) <= maximum,
      "missing, empty, nonregular or oversized program file");
}
bool Starts(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}
int RuntimeLibraries(dl_phdr_info* info, size_t, void* opaque) {
  // Hash actual loaded SDK files, including the private core, GPU compiler and
  // math runtimes. Paths also bind ancillary SDK resources to their install.
  auto& paths = *static_cast<std::set<std::string>*>(opaque);
  const fs::path path(info->dlpi_name);
  const auto name = path.filename().string();
  for (const auto* prefix :
       {"libmigraphx", "libamd", "libhip", "libhsa", "libroc", "libMIOpen", "libLLVM", "libclang", "libstdc++"}) {
    if (Starts(name, prefix)) {
      paths.insert(fs::canonical(path).string());
      break;
    }
  }
  return 0;
}
void Field(std::ostream& out, const std::string& value) {
  out << value.size() << ':' << value << '\n';
}
std::string RuntimeIdentity(int device) {
  hipDeviceProp_t props{};
  int runtime = 0, driver = 0;
  Require(
      hipGetDeviceProperties(&props, device) == hipSuccess && hipRuntimeGetVersion(&runtime) == hipSuccess &&
          hipDriverGetVersion(&driver) == hipSuccess,
      "cannot identify HIP device/runtime");
  struct utsname kernel {};
  Require(uname(&kernel) == 0, "cannot identify kernel");
  std::ostringstream identity;
  Field(identity, props.gcnArchName);
  Field(identity, props.name);
  identity << props.multiProcessorCount << '/' << props.warpSize << '/' << runtime << '/' << driver << '\n';
  Field(identity, kernel.release);
  std::set<std::string> libraries;
  dl_iterate_phdr(RuntimeLibraries, &libraries);
  bool gpu = false, compiler = false;
  for (const auto& library : libraries) {
    gpu |= fs::path(library).filename().string().find("libmigraphx_gpu") == 0;
    compiler |= fs::path(library).filename().string().find("libhiprtc") == 0;
    Field(identity, library);
    Field(identity, Digest(library));
  }
  Require(gpu && compiler, "GPU target and HIPRTC must be loaded before cache preparation");
  std::map<std::string, std::string> environment;
  for (char** entry = environ; *entry; ++entry) {
    const std::string value(*entry);
    const auto split = value.find('=');
    const auto name = value.substr(0, split);
    bool relevant = name == "LD_LIBRARY_PATH" || name == "LD_PRELOAD";
    for (const auto* prefix :
         {"MIGRAPHX_", "HIP_", "ROCM_", "ROCR_", "HSA_", "MIOPEN_", "ROCBLAS_", "HIPBLASLT_", "AMD_"})
      relevant |= Starts(name, prefix);
    if (relevant)
      environment.emplace(name, value.substr(split + 1));
  }
  // A caller-supplied compiler can change in place independently of HIPRTC.
  // Uncached compilation remains available for these development overrides.
  Require(!environment.count("MIGRAPHX_HIP_COMPILER"), "disable model_cache for a custom HIP compiler");
  for (const auto& [name, value] : environment) {
    Field(identity, name);
    Field(identity, value);
  }
  return identity.str();
}
void Save(const migraphx::program& program, const fs::path& path, size_t maximum) {
  migraphx::save(program, path.c_str());
  Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  BoundedFile(fd.value, maximum);
  Require(fsync(fd.value) == 0, "cannot sync program");
}
} // namespace

MigraphxCache::MigraphxCache(MigraphxCacheConfig config) : config_(std::move(config)) {}
MigraphxCache::~MigraphxCache() {
  std::error_code error;
  if (!scratch_.empty())
    fs::remove_all(scratch_, error);
}
std::unique_ptr<MigraphxCache> MigraphxCache::Prepare(
    const MigraphxCacheConfig& config,
    const migraphx::program& parsed,
    int device,
    bool fp16) {
  Require(
      config.directory.is_absolute() && config.maximum_entry_bytes > 0, "absolute directory and byte bound required");
  auto cache = std::unique_ptr<MigraphxCache>(new MigraphxCache(config));
  fs::create_directories(config.directory.parent_path());
  Require(mkdir(config.directory.c_str(), 0700) == 0 || errno == EEXIST, "cannot create cache directory");
  PrivateDirectory(config.directory);
  auto scratch = (config.directory / ".prepare-XXXXXX").string();
  Require(mkdtemp(scratch.data()) != nullptr, "cannot create preparation directory");
  cache->scratch_ = scratch;
  const auto path = cache->scratch_ / "program.mxr";
  Save(parsed, path, config.maximum_entry_bytes);
  // Schema version binds our parser/FP16/compile contract. Changing compilation
  // settings requires a new version. The parsed serialization contains shapes
  // and all loaded constants, including external ONNX tensor bytes.
  cache->key_ = DigestBytes(
      "hstream-migraphx-v1\noffload_copy=false\nfp16=" + std::to_string(fp16) + '\n' + Digest(path) + '\n' +
      RuntimeIdentity(device));
  fs::remove(path);
  return cache;
}
bool MigraphxCache::Load(migraphx::program& program) {
  const auto entry = config_.directory / key_;
  struct stat info {};
  if (lstat(entry.c_str(), &info) != 0) {
    Require(errno == ENOENT, "cannot inspect entry " + entry.string());
    return false;
  }
  PrivateDirectory(entry);
  Fd source(open((entry / "program.mxr").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  BoundedFile(source.value, config_.maximum_entry_bytes);
  Fd manifest(open((entry / "sha256").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  BoundedFile(manifest.value, 65);
  std::ifstream metadata(manifest.path());
  std::string expected((std::istreambuf_iterator<char>(metadata)), {});
  Require(expected.size() == 65 && expected.back() == '\n', "invalid checksum in " + entry.string());
  const auto owned = scratch_ / "program.mxr";
  fs::copy_file(source.path(), owned, fs::copy_options::overwrite_existing);
  Require(Digest(owned) + '\n' == expected, "corrupt entry; remove " + entry.string() + " and retry");
  program = migraphx::load(owned.c_str());
  return true;
}
void MigraphxCache::Publish(const migraphx::program& program) {
  const auto path = scratch_ / "program.mxr";
  Save(program, path, config_.maximum_entry_bytes);
  const auto digest = Digest(path) + '\n';
  Fd manifest(open((scratch_ / "sha256").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
  Require(
      manifest.value >= 0 && write(manifest.value, digest.data(), digest.size()) == ssize_t(digest.size()) &&
          fsync(manifest.value) == 0,
      "cannot write checksum");
  Fd directory(open(scratch_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  Require(directory.value >= 0 && fsync(directory.value) == 0, "cannot sync entry");
  const auto target = config_.directory / key_;
  // Complete entries are nonempty directories, which POSIX rename cannot
  // replace. This also works on filesystems lacking renameat2(NOREPLACE), such
  // as the ZFS cache used for Program qualification. Reject even empty existing
  // entries before the rename; concurrent publishers only expose nonempty ones.
  struct stat info {};
  if (lstat(target.c_str(), &info) == 0)
    return;
  Require(errno == ENOENT, "cannot inspect publication destination");
  if (rename(scratch_.c_str(), target.c_str()) != 0) {
    Require(errno == EEXIST || errno == ENOTEMPTY, "cannot publish " + target.string() + ": " + std::strerror(errno));
    return;
  }
  scratch_.clear();
  Fd root(open(config_.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  Require(root.value >= 0 && fsync(root.value) == 0, "cannot sync cache directory");
}
} // namespace hm::gpu
