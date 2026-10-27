#include "hstream/src/libs/stitching/RinkMaskInputStore.h"

#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <tiffio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/StatusMacros.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/TransactionState.h"

namespace hm::stitching {
namespace fs = std::filesystem;
struct RinkMaskInput::Storage {
  int descriptor{-1};
  struct stat identity{};
  std::string manifest;
  ~Storage() {
    if (descriptor >= 0)
      ::close(descriptor);
  }
};
namespace {
constexpr uint64_t kMaximumPixels = 256ULL * 1024 * 1024;
constexpr uint32_t kMaximumDimension = 65536;
constexpr size_t kMaximumManifestBytes = 64 * 1024;
constexpr const char* kOwnedMarker = ".rink-mask-input";
constexpr const char* kOwnedContents = "hstream-rink-mask-input-v1\n";

struct Descriptor {
  int value{-1};
  ~Descriptor() {
    if (value >= 0)
      ::close(value);
  }
};
using Tiff = std::unique_ptr<TIFF, decltype(&TIFFClose)>;
absl::Status io_error(const std::string& action) {
  return absl::InternalError(action + ": " + std::strerror(errno));
}
absl::Status cancelled(const std::function<bool()>& predicate) {
  return predicate && predicate() ? absl::CancelledError("Rink-mask input operation cancelled") : absl::OkStatus();
}
bool valid_id(const std::string& id) {
  return id.size() == 64 &&
      std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool same_file(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
      a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
      a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
class Digest {
 public:
  Digest() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
    ready_ = context_ && EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) == 1;
  }
  absl::Status Add(const void* data, size_t size) {
    if (!ready_ || EVP_DigestUpdate(context_.get(), data, size) != 1)
      return absl::InternalError("Cannot hash rink-mask input");
    return absl::OkStatus();
  }
  absl::StatusOr<std::string> Finish() {
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (!ready_ || EVP_DigestFinal_ex(context_.get(), bytes, &size) != 1 || size != 32)
      return absl::InternalError("Cannot finish rink-mask input hash");
    std::ostringstream out;
    for (unsigned int i = 0; i < size; ++i)
      out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(bytes[i]);
    return out.str();
  }

 private:
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
  bool ready_{false};
};
absl::StatusOr<std::string> hash_text(const std::string& text) {
  Digest digest;
  HM_RETURN_IF_ERROR(digest.Add(text.data(), text.size()));
  return digest.Finish();
}
absl::StatusOr<std::string> hash_file(int fd, const struct stat& before, const std::function<bool()>& stop) {
  Digest digest;
  std::array<char, 64 * 1024> buffer;
  uint64_t position = 0;
  while (position < static_cast<uint64_t>(before.st_size)) {
    HM_RETURN_IF_ERROR(cancelled(stop));
    const ssize_t count =
        ::pread(fd, buffer.data(), std::min<uint64_t>(buffer.size(), before.st_size - position), position);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return absl::DataLossError("Truncated rink-mask input");
    HM_RETURN_IF_ERROR(digest.Add(buffer.data(), count));
    position += count;
  }
  struct stat after{};
  if (::fstat(fd, &after) != 0)
    return io_error("Cannot recheck rink-mask input");
  if (!same_file(before, after))
    return absl::AbortedError("Rink-mask input changed while reading");
  return digest.Finish();
}
size_t pixel_bytes(const RinkMaskInputDescription& d) {
  return d.format == "half4-bits" ? 8 : 4;
}
absl::Status validate(const RinkMaskInputDescription& d, const RinkMaskInputLimits& limits) {
  if (!limits.max_raw_bytes || !limits.max_file_bytes)
    return absl::InvalidArgumentError("Rink-mask input byte limits must be positive");
  if (!d.width || !d.height || d.width > kMaximumDimension || d.height > kMaximumDimension ||
      uint64_t(d.width) * d.height > kMaximumPixels ||
      uint64_t(d.width) * d.height * pixel_bytes(d) > limits.max_raw_bytes)
    return absl::ResourceExhaustedError("Rink-mask input dimensions exceed the supported limits");
  if (d.format != "rgba8" && d.format != "half4-bits")
    return absl::InvalidArgumentError("Unsupported rink-mask input pixel format");
  if (d.hugin_generation.empty() || d.hugin_generation.size() > 4096 || !valid_id(d.hugin_fingerprint) ||
      d.frame.selection.empty() || d.frame.selection.size() > 64 || d.frame.selection == "auto" ||
      !d.frame.recording_time_ns)
    return absl::InvalidArgumentError("Rink-mask input requires a Hugin identity and a selected recording position");
  for (const auto& source : d.frame.sources)
    if (source.video.empty() || source.video.string().size() > 8192 || !std::isfinite(source.seconds) ||
        source.seconds < 0)
      return absl::InvalidArgumentError("Rink-mask input requires both original camera samples");
  return absl::OkStatus();
}
absl::Status sync_path(const fs::path& path, bool directory = false) {
  Descriptor fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (directory ? O_DIRECTORY : 0))};
  if (fd.value < 0 || ::fsync(fd.value) != 0)
    return io_error("Cannot sync rink-mask input");
  return absl::OkStatus();
}
absl::Status write_file(const fs::path& path, const std::string& text) {
  Descriptor fd{::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
  if (fd.value < 0)
    return io_error("Cannot create rink-mask input manifest");
  size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t count = ::write(fd.value, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return io_error("Cannot write rink-mask input manifest");
    offset += count;
  }
  if (::fsync(fd.value) != 0)
    return io_error("Cannot sync rink-mask input manifest");
  return absl::OkStatus();
}
// A small, explicitly sampled source identity is portable across private copies.
// It is historical provenance, not a claim that a whole video was hashed.
absl::StatusOr<YAML::Node> source_identity(const CalibrationFrameSource& source) {
  // Private media aliases are legitimate source inputs. Resolve once, then
  // pin the resulting regular inode; retain the original alias as provenance.
  std::error_code path_error;
  const fs::path resolved = fs::canonical(source.video, path_error);
  if (path_error)
    return absl::FailedPreconditionError("Cannot resolve sampled rink-mask source: " + path_error.message());
  Descriptor fd{::open(resolved.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  if (fd.value < 0)
    return io_error("Cannot open sampled rink-mask source");
  struct stat before{};
  if (::fstat(fd.value, &before) != 0)
    return io_error("Cannot stat sampled rink-mask source");
  if (!S_ISREG(before.st_mode) || before.st_size <= 0)
    return absl::FailedPreconditionError("Sampled rink-mask source is not a regular video");
  Digest digest;
  std::array<char, 64 * 1024> buffer;
  const uint64_t bytes = before.st_size;
  for (uint64_t offset : {uint64_t(0), bytes > buffer.size() ? bytes - buffer.size() : uint64_t(0)}) {
    const size_t wanted = std::min<uint64_t>(buffer.size(), bytes - offset);
    const ssize_t got = ::pread(fd.value, buffer.data(), wanted, offset);
    if (got != static_cast<ssize_t>(wanted))
      return absl::AbortedError("Sampled source changed while reading");
    HM_RETURN_IF_ERROR(digest.Add(buffer.data(), wanted));
  }
  struct stat after{};
  if (::fstat(fd.value, &after) != 0 || !same_file(before, after))
    return absl::AbortedError("Sampled source changed while reading");
  std::string hash;
  HM_ASSIGN_OR_RETURN(hash, digest.Finish());
  YAML::Node result;
  result["video"] = source.video.string();
  result["seconds"] = source.seconds;
  result["bytes"] = bytes;
  result["sample_sha256"] = hash;
  return result;
}
struct Staging {
  fs::path path;
  ~Staging() {
    if (!path.empty())
      (void)remove_owned_directory(path, kOwnedMarker, kOwnedContents);
  }
};
absl::StatusOr<fs::path> create_staging(const fs::path& game) {
  auto pinned = PinnedDirectory::Open(game, "rink-mask input game");
  if (!pinned.ok())
    return pinned.status();
  if (::mkdirat(pinned->descriptor(), "rink-mask-input", 0700) != 0 && errno != EEXIST)
    return io_error("Cannot create rink-mask input store");
  auto store = pinned->OpenChild("rink-mask-input", "rink-mask input store");
  if (!store.ok())
    return store.status();
  if (!store->has_value())
    return absl::NotFoundError("Rink-mask input store disappeared");
  // All parent components were pinned/validated above; callers own this private game.
  std::string pattern = (game / "rink-mask-input" / ".pending-XXXXXX").string();
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back(0);
  char* created = ::mkdtemp(name.data());
  if (!created)
    return io_error("Cannot stage rink-mask input");
  fs::path path(created);
  HM_RETURN_IF_ERROR(write_owned_directory_marker(path, kOwnedMarker, kOwnedContents));
  return path;
}
absl::Status publish_staging(Staging& staged, const fs::path& game, const std::string& id) {
  HM_RETURN_IF_ERROR(sync_path(staged.path, true));
  const fs::path target = game / "rink-mask-input" / id;
  if (::rename(staged.path.c_str(), target.c_str()) != 0) {
    if (errno != EEXIST && errno != ENOTEMPTY)
      return io_error("Cannot publish rink-mask input");
  } else {
    staged.path.clear();
  }
  HM_RETURN_IF_ERROR(sync_path(game / "rink-mask-input", true));
  return sync_path(game, true);
}
absl::Status check_tiff(TIFF* tiff, const RinkMaskInputDescription& d) {
  uint32_t width = 0, height = 0, rows = 0;
  uint16_t samples = 0, bits = 0, planar = 0, sample_format = 0, compression = 0, orientation = 0;
  if (!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) || !TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) ||
      !TIFFGetField(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples) || !TIFFGetField(tiff, TIFFTAG_BITSPERSAMPLE, &bits) ||
      !TIFFGetField(tiff, TIFFTAG_ROWSPERSTRIP, &rows) || !TIFFGetField(tiff, TIFFTAG_PLANARCONFIG, &planar) ||
      !TIFFGetField(tiff, TIFFTAG_SAMPLEFORMAT, &sample_format) ||
      !TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression) ||
      !TIFFGetField(tiff, TIFFTAG_ORIENTATION, &orientation) || width != d.width || height != d.height ||
      samples != 4 || bits != pixel_bytes(d) * 2 || rows != 1 || planar != PLANARCONFIG_CONTIG ||
      sample_format != SAMPLEFORMAT_UINT || orientation != ORIENTATION_TOPLEFT || TIFFIsTiled(tiff) ||
      (compression != COMPRESSION_ADOBE_DEFLATE && compression != COMPRESSION_NONE) ||
      TIFFScanlineSize64(tiff) != uint64_t(d.width) * pixel_bytes(d))
    return absl::FailedPreconditionError("Rink-mask TIFF geometry or encoding differs from its manifest");
  return absl::OkStatus();
}
} // namespace

absl::StatusOr<RinkMaskInputLimits> ReadRinkMaskInputLimits(const YAML::Node& config) {
  try {
    auto values = [](const YAML::Node& root) {
      if (!root || !root.IsMap() || !root["stitching"] || !root["stitching"].IsMap() ||
          !root["stitching"]["rink_mask_input"])
        return YAML::Node();
      return root["stitching"]["rink_mask_input"];
    };
    YAML::Node node = values(config);
    YAML::Node baseline;
    if (!node.IsMap() || !node["max_raw_bytes"] || !node["max_file_bytes"]) {
      auto loaded = hm::baseline_config::load();
      if (!loaded.ok())
        return loaded.status();
      baseline = values(loaded->values);
    }
    const auto value = [&](const char* key) {
      return node.IsMap() && node[key] ? node[key].as<uint64_t>() : baseline[key].as<uint64_t>();
    };
    RinkMaskInputLimits limits{value("max_raw_bytes"), value("max_file_bytes")};
    if (!limits.max_raw_bytes || !limits.max_file_bytes)
      return absl::InvalidArgumentError("stitching.rink_mask_input requires positive byte limits");
    return limits;
  } catch (const YAML::Exception& e) {
    return absl::InvalidArgumentError(
        "Required stitching.rink_mask_input limits are invalid: " + std::string(e.what()));
  }
}

absl::StatusOr<RinkMaskInput> PublishRinkMaskInput(
    const fs::path& game,
    const RinkMaskInputDescription& d,
    const RinkMaskInputLimits& limits,
    const RinkMaskInputReadRow& read_row,
    const std::function<bool()>& stop) {
  HM_RETURN_IF_ERROR(validate(d, limits));
  if (!read_row)
    return absl::InvalidArgumentError("Rink-mask input row producer is required");
  HM_RETURN_IF_ERROR(cancelled(stop));
  Staging staged;
  HM_ASSIGN_OR_RETURN(staged.path, create_staging(game));
  const fs::path payload = staged.path / "input.tiff";
  Descriptor fd{::open(payload.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
  if (fd.value < 0)
    return io_error("Cannot create rink-mask input TIFF");
  if (::flock(fd.value, LOCK_SH) != 0)
    return io_error("Cannot lease rink-mask input");
  Tiff tiff(TIFFFdOpen(::dup(fd.value), payload.c_str(), "w"), TIFFClose);
  if (!tiff)
    return absl::InternalError("Cannot initialize rink-mask input TIFF");
  uint16_t extra = EXTRASAMPLE_UNASSALPHA;
  if (!TIFFSetField(tiff.get(), TIFFTAG_IMAGEWIDTH, d.width) ||
      !TIFFSetField(tiff.get(), TIFFTAG_IMAGELENGTH, d.height) ||
      !TIFFSetField(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, 4) ||
      !TIFFSetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, pixel_bytes(d) * 2) ||
      !TIFFSetField(tiff.get(), TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT) ||
      !TIFFSetField(tiff.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) ||
      !TIFFSetField(tiff.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB) ||
      !TIFFSetField(tiff.get(), TIFFTAG_EXTRASAMPLES, 1, &extra) ||
      !TIFFSetField(tiff.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) ||
      !TIFFSetField(tiff.get(), TIFFTAG_ROWSPERSTRIP, 1) ||
      !TIFFSetField(
          tiff.get(),
          TIFFTAG_COMPRESSION,
          TIFFIsCODECConfigured(COMPRESSION_ADOBE_DEFLATE) ? COMPRESSION_ADOBE_DEFLATE : COMPRESSION_NONE))
    return absl::InternalError("Cannot configure rink-mask input TIFF");
  std::vector<uint16_t> row((uint64_t(d.width) * pixel_bytes(d) + 1) / 2);
  for (uint32_t y = 0; y < d.height; ++y) {
    HM_RETURN_IF_ERROR(cancelled(stop));
    HM_RETURN_IF_ERROR(read_row(y, row.data(), uint64_t(d.width) * pixel_bytes(d)));
    if (TIFFWriteScanline(tiff.get(), row.data(), y, 0) < 0)
      return absl::InternalError("Cannot write rink-mask input row");
    struct stat size{};
    if (::fstat(fd.value, &size) != 0)
      return io_error("Cannot bound rink-mask input");
    if (static_cast<uint64_t>(size.st_size) > limits.max_file_bytes)
      return absl::ResourceExhaustedError("Encoded rink-mask input exceeds its byte limit");
  }
  tiff.reset();
  if (::fsync(fd.value) != 0)
    return io_error("Cannot sync rink-mask input TIFF");
  struct stat size{};
  if (::fstat(fd.value, &size) != 0)
    return io_error("Cannot stat rink-mask input TIFF");
  if (size.st_size <= 0 || static_cast<uint64_t>(size.st_size) > limits.max_file_bytes)
    return absl::ResourceExhaustedError("Encoded rink-mask input exceeds its byte limit");
  std::string hash;
  HM_ASSIGN_OR_RETURN(hash, hash_file(fd.value, size, stop));
  YAML::Node manifest;
  manifest["version"] = 1;
  manifest["width"] = d.width;
  manifest["height"] = d.height;
  manifest["format"] = d.format;
  manifest["hugin_generation"] = d.hugin_generation;
  manifest["hugin_fingerprint"] = d.hugin_fingerprint;
  manifest["frame"]["selection"] = d.frame.selection;
  manifest["frame"]["recording_time_ns"] = *d.frame.recording_time_ns;
  for (const auto& source : d.frame.sources) {
    YAML::Node identity;
    HM_ASSIGN_OR_RETURN(identity, source_identity(source));
    manifest["frame"]["sources"].push_back(identity);
  }
  manifest["payload"]["bytes"] = static_cast<uint64_t>(size.st_size);
  manifest["payload"]["sha256"] = hash;
  const std::string text = YAML::Dump(manifest) + "\n";
  if (text.size() > kMaximumManifestBytes)
    return absl::ResourceExhaustedError("Rink-mask manifest is too large");
  std::string id;
  HM_ASSIGN_OR_RETURN(id, hash_text(text));
  HM_RETURN_IF_ERROR(write_file(staged.path / "manifest.yaml", text));
  HM_RETURN_IF_ERROR(cancelled(stop));
  HM_RETURN_IF_ERROR(publish_staging(staged, game, id));
  return InspectRinkMaskInput(game, id, limits, stop);
}

absl::StatusOr<RinkMaskInput> InspectRinkMaskInput(
    const fs::path& game,
    const std::string& id,
    const RinkMaskInputLimits& limits,
    const std::function<bool()>& stop) {
  if (!valid_id(id))
    return absl::InvalidArgumentError("Invalid rink-mask input reference");
  auto root = PinnedDirectory::Open(game, "rink-mask input game");
  if (!root.ok())
    return root.status();
  auto store = root->OpenChild("rink-mask-input", "rink-mask input store");
  if (!store.ok())
    return store.status();
  if (!store->has_value())
    return absl::NotFoundError("No retained rink-mask input store");
  auto bundle = (**store).OpenChild(id, "rink-mask input bundle");
  if (!bundle.ok())
    return bundle.status();
  if (!bundle->has_value())
    return absl::NotFoundError("Retained rink-mask input is missing");
  auto text = read_bounded_regular_file_no_follow(
      (**bundle).path() / "manifest.yaml", kMaximumManifestBytes, "rink-mask manifest");
  if (!text.ok())
    return text.status();
  std::string actual_id;
  HM_ASSIGN_OR_RETURN(actual_id, hash_text(*text));
  if (actual_id != id)
    return absl::DataLossError("Rink-mask input manifest differs from its reference");
  try {
    const YAML::Node manifest = YAML::Load(*text);
    if (manifest["version"].as<int>() != 1)
      return absl::InvalidArgumentError("Unsupported rink-mask input version");
    RinkMaskInput input;
    input.id = id;
    auto& d = input.description;
    d.width = manifest["width"].as<uint32_t>();
    d.height = manifest["height"].as<uint32_t>();
    d.format = manifest["format"].as<std::string>();
    d.hugin_generation = manifest["hugin_generation"].as<std::string>();
    d.hugin_fingerprint = manifest["hugin_fingerprint"].as<std::string>();
    d.frame.selection = manifest["frame"]["selection"].as<std::string>();
    d.frame.recording_time_ns = manifest["frame"]["recording_time_ns"].as<uint64_t>();
    d.frame.input_id = id;
    const YAML::Node sources = manifest["frame"]["sources"];
    if (!sources.IsSequence() || sources.size() != 2)
      return absl::InvalidArgumentError("Rink-mask input camera provenance is incomplete");
    for (size_t i = 0; i < 2; ++i) {
      d.frame.sources[i].video = sources[i]["video"].as<std::string>();
      d.frame.sources[i].seconds = sources[i]["seconds"].as<double>();
      if (!sources[i]["bytes"].as<uint64_t>() || !valid_id(sources[i]["sample_sha256"].as<std::string>()))
        return absl::InvalidArgumentError("Rink-mask input source sample identity is invalid");
    }
    HM_RETURN_IF_ERROR(validate(d, limits));
    input.file_bytes = manifest["payload"]["bytes"].as<uint64_t>();
    const std::string hash = manifest["payload"]["sha256"].as<std::string>();
    if (!input.file_bytes || input.file_bytes > limits.max_file_bytes || !valid_id(hash))
      return absl::ResourceExhaustedError("Rink-mask input payload identity exceeds its limits");
    input.storage = std::make_shared<RinkMaskInput::Storage>();
    auto& storage = *input.storage;
    storage.manifest = *text;
    storage.descriptor =
        ::openat((**bundle).descriptor(), "input.tiff", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (storage.descriptor < 0)
      return io_error("Cannot open retained rink-mask input");
    if (::flock(storage.descriptor, LOCK_SH) != 0 || ::fstat(storage.descriptor, &storage.identity) != 0)
      return io_error("Cannot lease retained rink-mask input");
    if (!S_ISREG(storage.identity.st_mode) || storage.identity.st_size <= 0 ||
        static_cast<uint64_t>(storage.identity.st_size) != input.file_bytes)
      return absl::DataLossError("Retained rink-mask input size differs from its manifest");
    std::string actual;
    HM_ASSIGN_OR_RETURN(actual, hash_file(storage.descriptor, storage.identity, stop));
    if (actual != hash)
      return absl::DataLossError("Retained rink-mask input checksum mismatch");
    Tiff tiff(TIFFFdOpen(::dup(storage.descriptor), "rink-mask input", "r"), TIFFClose);
    if (!tiff)
      return absl::DataLossError("Cannot read retained rink-mask TIFF");
    HM_RETURN_IF_ERROR(check_tiff(tiff.get(), d));
    return input;
  } catch (const YAML::Exception& e) {
    return absl::InvalidArgumentError("Invalid rink-mask input manifest: " + std::string(e.what()));
  }
}

absl::StatusOr<RinkMaskInput> LoadRinkMaskInputForConfig(
    const fs::path& game,
    const YAML::Node& config,
    const std::string& hugin_generation,
    const std::string& hugin_fingerprint,
    uint32_t width,
    uint32_t height,
    const std::function<bool()>& stop) {
  HM_RETURN_IF_ERROR(cancelled(stop));
  try {
    if (!config.IsMap() || !config["rink"] || !config["rink"].IsMap() || !config["rink"]["mask_frame"])
      return absl::NotFoundError("The selected rink-mask frame has no retained original input");
    const YAML::Node frame = config["rink"]["mask_frame"];
    if (!frame.IsMap() || !frame["input_id"] || !frame["input_id"].IsScalar())
      return absl::NotFoundError("The selected rink-mask frame has no retained original input");
    RinkMaskInputLimits limits;
    HM_ASSIGN_OR_RETURN(limits, ReadRinkMaskInputLimits(config));
    RinkMaskInput input;
    HM_ASSIGN_OR_RETURN(input, InspectRinkMaskInput(game, frame["input_id"].as<std::string>(), limits, stop));
    std::string selection;
    HM_ASSIGN_OR_RETURN(selection, read_rink_mask_frame_time(config));
    const auto& d = input.description;
    if (d.hugin_generation != hugin_generation || d.hugin_fingerprint != hugin_fingerprint ||
        (width && d.width != width) || (height && d.height != height) || d.frame.selection != selection ||
        frame["selection"].as<std::string>() != selection ||
        frame["recording_time_ns"].as<uint64_t>() != *d.frame.recording_time_ns)
      return absl::FailedPreconditionError(
          "Retained rink-mask input belongs to another calibration or frame selection");
    const YAML::Node sources = frame["sources"];
    if (!sources.IsSequence() || sources.size() != 2)
      return absl::FailedPreconditionError("Retained rink-mask input has no matching sampled camera provenance");
    for (size_t i = 0; i < 2; ++i)
      if (sources[i]["seconds"].as<double>() != d.frame.sources[i].seconds)
        return absl::FailedPreconditionError("Retained rink-mask input camera positions changed");
    return input;
  } catch (const YAML::Exception& e) {
    return absl::InvalidArgumentError("Invalid retained rink-mask reference: " + std::string(e.what()));
  }
}

absl::Status ReadRinkMaskInputRows(
    const RinkMaskInput& input,
    const RinkMaskInputWriteRow& write_row,
    const std::function<bool()>& stop) {
  if (!input.storage || !write_row)
    return absl::InvalidArgumentError("Validated rink-mask input and row consumer are required");
  struct stat current{};
  const auto& storage = *input.storage;
  if (::fstat(storage.descriptor, &current) != 0 || !same_file(storage.identity, current))
    return absl::AbortedError("Retained rink-mask input changed after validation");
  // dup shares offsets; reset before each independent libtiff handle.
  if (::lseek(storage.descriptor, 0, SEEK_SET) < 0)
    return io_error("Cannot rewind retained rink-mask input");
  Tiff tiff(TIFFFdOpen(::dup(storage.descriptor), "rink-mask input", "r"), TIFFClose);
  if (!tiff)
    return absl::DataLossError("Cannot open retained rink-mask TIFF");
  HM_RETURN_IF_ERROR(check_tiff(tiff.get(), input.description));
  const size_t bytes = uint64_t(input.description.width) * pixel_bytes(input.description);
  std::vector<uint16_t> row((bytes + 1) / 2);
  for (uint32_t y = 0; y < input.description.height; ++y) {
    HM_RETURN_IF_ERROR(cancelled(stop));
    if (TIFFReadScanline(tiff.get(), row.data(), y, 0) < 0)
      return absl::DataLossError("Cannot decode retained rink-mask row");
    HM_RETURN_IF_ERROR(write_row(y, row.data(), bytes));
  }
  if (::fstat(storage.descriptor, &current) != 0 || !same_file(storage.identity, current))
    return absl::AbortedError("Retained rink-mask input changed while decoding");
  return absl::OkStatus();
}

absl::Status CopyRinkMaskInput(
    const fs::path& source,
    const fs::path& destination,
    const std::string& id,
    const RinkMaskInputLimits& limits) {
  RinkMaskInput input;
  HM_ASSIGN_OR_RETURN(input, InspectRinkMaskInput(source, id, limits));
  Staging staged;
  HM_ASSIGN_OR_RETURN(staged.path, create_staging(destination));
  Descriptor fd{
      ::open((staged.path / "input.tiff").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
  if (fd.value < 0)
    return io_error("Cannot create copied rink-mask input");
  std::array<char, 64 * 1024> buffer;
  for (uint64_t offset = 0; offset < input.file_bytes;) {
    const ssize_t count = ::pread(
        input.storage->descriptor, buffer.data(), std::min<uint64_t>(buffer.size(), input.file_bytes - offset), offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return absl::AbortedError("Retained rink-mask input changed while copying");
    ssize_t written = 0;
    while (written < count) {
      const ssize_t result = ::write(fd.value, buffer.data() + written, count - written);
      if (result < 0 && errno == EINTR)
        continue;
      if (result <= 0)
        return io_error("Cannot copy retained rink-mask input");
      written += result;
    }
    offset += count;
  }
  if (::fsync(fd.value) != 0)
    return io_error("Cannot sync copied rink-mask input");
  struct stat current{};
  if (::fstat(input.storage->descriptor, &current) != 0 || !same_file(input.storage->identity, current))
    return absl::AbortedError("Retained rink-mask input changed while copying");
  HM_RETURN_IF_ERROR(write_file(staged.path / "manifest.yaml", input.storage->manifest));
  HM_RETURN_IF_ERROR(publish_staging(staged, destination, id));
  auto checked = InspectRinkMaskInput(destination, id, limits);
  return checked.ok() ? absl::OkStatus() : checked.status();
}

absl::StatusOr<std::vector<fs::path>> RinkMaskInputArtifacts(const std::string& id) {
  if (!valid_id(id))
    return absl::InvalidArgumentError("Invalid rink-mask input reference");
  const fs::path root = fs::path("rink-mask-input") / id;
  return std::vector<fs::path>{root / "manifest.yaml", root / "input.tiff", root / kOwnedMarker};
}

absl::Status PruneRinkMaskInputs(const fs::path& game, const std::string& keep_id) {
  auto root = PinnedDirectory::Open(game, "rink-mask input game");
  if (!root.ok())
    return root.status();
  auto store = root->OpenChild("rink-mask-input", "rink-mask input store");
  if (!store.ok())
    return store.status();
  if (!store->has_value())
    return absl::OkStatus();
  std::error_code error;
  fs::directory_iterator it((**store).path(), error);
  if (error)
    return absl::InternalError("Cannot enumerate rink-mask inputs: " + error.message());
  for (const auto& entry : it) {
    const std::string name = entry.path().filename().string();
    if (name == keep_id || !valid_id(name))
      continue;
    auto child = (**store).OpenChild(name, "obsolete rink-mask input");
    if (!child.ok() || !child->has_value())
      continue;
    Descriptor fd{::openat((**child).descriptor(), "input.tiff", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (fd.value < 0 || ::flock(fd.value, LOCK_EX | LOCK_NB) != 0)
      continue;
    HM_RETURN_IF_ERROR(remove_owned_directory((**store).path() / name, kOwnedMarker, kOwnedContents));
  }
  return absl::OkStatus();
}
} // namespace hm::stitching
