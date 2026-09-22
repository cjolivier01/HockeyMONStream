#include "hstream/src/libs/stitching/PlayerFrameInputStore.h"

#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/TransactionState.h"

namespace hm::stitching {
namespace {
namespace fs = std::filesystem;
constexpr size_t kMaximumManifestBytes = 256 * 1024;
constexpr size_t kMaximumImageBytes = 512ULL * 1024 * 1024;
constexpr size_t kMaximumThumbnailBytes = 4 * 1024 * 1024;
constexpr uint64_t kMaximumImagePixels = 128ULL * 1024 * 1024;

struct Descriptor {
  int value{-1};
  ~Descriptor() {
    if (value >= 0)
      ::close(value);
  }
};

absl::Status io_error(const std::string& action) {
  return absl::InternalError(action + ": " + std::strerror(errno));
}

std::string name(size_t index, size_t camera, const char* extension) {
  return std::string(camera == 0 ? "left_" : "right_") + std::to_string(index) + extension;
}

// Stream a bounded regular inode. In particular, do not read a full 8K image
// into memory merely to hash it, and never block opening a replaced FIFO.
absl::StatusOr<YAML::Node> file_identity(const fs::path& path, size_t maximum_bytes, bool png) {
  Descriptor descriptor{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  if (descriptor.value < 0)
    return io_error("Cannot open saved frame " + path.string());
  struct stat before{};
  if (::fstat(descriptor.value, &before) != 0)
    return io_error("Cannot stat saved frame");
  if (!S_ISREG(before.st_mode) || before.st_size <= 0 || static_cast<uint64_t>(before.st_size) > maximum_bytes)
    return absl::FailedPreconditionError("Saved frame is not a bounded regular image: " + path.string());
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
    return absl::InternalError("Cannot initialize saved frame digest");
  std::array<unsigned char, 64 * 1024> buffer{};
  uint64_t total = 0;
  YAML::Node result;
  for (;;) {
    const ssize_t count = ::read(descriptor.value, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0)
      return io_error("Cannot read saved frame");
    if (count == 0)
      break;
    if (total == 0 && png) {
      const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
      if (count < 33 || std::memcmp(buffer.data(), signature, sizeof(signature)) != 0 ||
          std::memcmp(buffer.data() + 12, "IHDR", 4) != 0)
        return absl::FailedPreconditionError("Saved input has an invalid PNG header");
      const auto number = [&](size_t offset) -> uint32_t {
        return (uint32_t(buffer[offset]) << 24) | (uint32_t(buffer[offset + 1]) << 16) |
            (uint32_t(buffer[offset + 2]) << 8) | uint32_t(buffer[offset + 3]);
      };
      const uint32_t width = number(16), height = number(20);
      const unsigned int depth = buffer[24], color = buffer[25];
      if (!width || !height || width > 32768 || height > 32768 || uint64_t(width) * height > kMaximumImagePixels ||
          (depth != 8 && depth != 16) || (color != 2 && color != 6))
        return absl::FailedPreconditionError("Saved input PNG dimensions or format exceed calibration limits");
      result["width"] = width;
      result["height"] = height;
      result["depth"] = depth;
      result["color"] = color;
    }
    total += count;
    if (total > maximum_bytes || EVP_DigestUpdate(digest.get(), buffer.data(), count) != 1)
      return absl::FailedPreconditionError("Saved frame changed or could not be hashed");
  }
  struct stat after{};
  if (::fstat(descriptor.value, &after) != 0)
    return io_error("Cannot recheck saved frame");
  if (total != static_cast<uint64_t>(before.st_size) || before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
    return absl::AbortedError("Saved frame changed during validation");
  std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(digest.get(), bytes.data(), &length) != 1 || length != 32)
    return absl::InternalError("Cannot finalize saved frame digest");
  std::ostringstream encoded;
  for (unsigned int index = 0; index < length; ++index)
    encoded << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(bytes[index]);
  result["bytes"] = total;
  result["sha256"] = encoded.str();
  return result;
}

absl::Status verify_file(const fs::path& path, const YAML::Node& expected, bool png) {
  if (!expected.IsMap())
    return absl::InvalidArgumentError("Saved frame manifest has no image identity");
  YAML::Node actual;
  HM_ASSIGN_OR_RETURN(actual, file_identity(path, png ? kMaximumImageBytes : kMaximumThumbnailBytes, png));
  for (const char* field : {"bytes", "sha256", "width", "height", "depth", "color"}) {
    if (!png && std::string(field) != "bytes" && std::string(field) != "sha256")
      continue;
    if (!expected[field] || expected[field].as<std::string>() != actual[field].as<std::string>())
      return absl::FailedPreconditionError("Saved frame content does not match its manifest: " + path.string());
  }
  return absl::OkStatus();
}
} // namespace

absl::StatusOr<std::optional<PlayerFrameInputSet>> LoadPlayerFrameInputs(
    const fs::path& game_directory,
    const PlayerFrameSelectionPlan& plan,
    PlayerFrameInputValidation validation) {
  HM_RETURN_IF_ERROR(ValidatePlayerFrameSelectionPlan(plan));
  const fs::path root = game_directory / "player-frame-inputs";
  auto game = PinnedDirectory::Open(game_directory, "saved frame game");
  if (!game.ok())
    return game.status();
  auto store = game->OpenChild("player-frame-inputs", "saved frame store");
  if (!store.ok())
    return store.status();
  if (!store->has_value())
    return std::nullopt;
  auto child = (**store).OpenChild(plan.fingerprint, "saved player frame bundle");
  if (!child.ok())
    return child.status();
  if (!child->has_value())
    return std::nullopt;
  auto& pinned = **child;
  std::string contents;
  auto manifest_contents =
      read_bounded_regular_file_no_follow(pinned.path() / "frames.yaml", kMaximumManifestBytes, "saved frame manifest");
  if (!manifest_contents.ok())
    return absl::FailedPreconditionError(
        "Cannot read existing saved frame manifest: " + manifest_contents.status().ToString());
  contents = std::move(*manifest_contents);
  try {
    const YAML::Node manifest = YAML::Load(contents);
    if (!manifest.IsMap() || manifest["version"].as<int>(0) != 1 ||
        manifest["fingerprint"].as<std::string>("") != plan.fingerprint)
      return absl::FailedPreconditionError("Saved frame manifest has the wrong version or selection owner");
    PlayerFrameSelectionPlan stored;
    HM_ASSIGN_OR_RETURN(stored, ParsePlayerFrameSelectionPlan(manifest["plan"]));
    if (stored.fingerprint != plan.fingerprint)
      return absl::FailedPreconditionError("Saved frame manifest identifies another selection");
    const YAML::Node pairs = manifest["pairs"];
    if (!pairs.IsSequence() || pairs.size() != plan.selected.size())
      return absl::FailedPreconditionError("Saved frame bundle is incomplete");
    PlayerFrameInputSet result;
    for (size_t index = 0; index < pairs.size(); ++index) {
      std::array<fs::path, 2> images, thumbnails;
      std::array<std::string, 2> image_digests, thumbnail_digests;
      for (size_t camera = 0; camera < 2; ++camera) {
        const YAML::Node item = pairs[index][camera == 0 ? "left" : "right"];
        const std::string png = name(index, camera, ".png"), jpg = name(index, camera, ".jpg");
        if (validation == PlayerFrameInputValidation::kFull)
          HM_RETURN_IF_ERROR(verify_file(pinned.path() / png, item["image"], true));
        HM_RETURN_IF_ERROR(verify_file(pinned.path() / jpg, item["thumbnail"], false));
        images[camera] = root / plan.fingerprint / png;
        thumbnails[camera] = root / plan.fingerprint / jpg;
        image_digests[camera] = item["image"]["sha256"].as<std::string>();
        thumbnail_digests[camera] = item["thumbnail"]["sha256"].as<std::string>();
      }
      result.images.push_back(std::move(images));
      result.thumbnails.push_back(std::move(thumbnails));
      result.image_digests.push_back(std::move(image_digests));
      result.thumbnail_digests.push_back(std::move(thumbnail_digests));
    }
    return result;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Invalid saved frame manifest: " + std::string(error.what()));
  }
}

static absl::Status publish_player_frame_inputs(
    const fs::path& game_directory,
    const PlayerFrameSelectionPlan& plan,
    const std::vector<std::array<fs::path, 2>>& images,
    const PlayerFrameInputSet* saved) {
  HM_RETURN_IF_ERROR(ValidatePlayerFrameSelectionPlan(plan));
  if (images.size() != plan.selected.size())
    return absl::InvalidArgumentError("Cannot persist an incomplete selected frame set");
  std::error_code error;
  auto game = PinnedDirectory::Open(game_directory, "saved frame game");
  if (!game.ok())
    return game.status();
  if (::mkdirat(game->descriptor(), "player-frame-inputs", 0700) != 0 && errno != EEXIST)
    return io_error("Cannot create saved frame store");
  auto child = game->OpenChild("player-frame-inputs", "saved frame store");
  if (!child.ok())
    return child.status();
  if (!child->has_value())
    return absl::AbortedError("Saved frame store disappeared during publication");
  auto* pinned = &**child;
  Descriptor lock{::open((pinned->path() / ".lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600)};
  if (lock.value < 0)
    return io_error("Cannot open saved frame store lock");
  struct stat lock_stat{};
  if (::fstat(lock.value, &lock_stat) != 0 || !S_ISREG(lock_stat.st_mode))
    return absl::FailedPreconditionError("Saved frame store lock is not a regular file");
  while (::flock(lock.value, LOCK_EX) != 0) {
    if (errno != EINTR)
      return io_error("Cannot lock saved frame store");
  }
  auto existing = LoadPlayerFrameInputs(game_directory, plan);
  if (!existing.ok())
    return existing.status();
  if (existing->has_value()) {
    for (size_t index = 0; index < images.size(); ++index) {
      for (size_t camera = 0; camera < 2; ++camera) {
        YAML::Node source_identity;
        HM_ASSIGN_OR_RETURN(source_identity, file_identity(images[index][camera], kMaximumImageBytes, true));
        HM_RETURN_IF_ERROR(verify_file((**existing).images[index][camera], source_identity, true));
      }
    }
    return absl::OkStatus();
  }
  std::string pattern = (pinned->path() / ".staging-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const char* created = ::mkdtemp(writable.data());
  if (!created)
    return io_error("Cannot stage saved frame bundle");
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } staging{created};
  try {
    YAML::Node manifest;
    manifest["version"] = 1;
    manifest["fingerprint"] = plan.fingerprint;
    manifest["plan"] = PlayerFrameSelectionPlanYaml(plan);
    for (size_t index = 0; index < images.size(); ++index) {
      YAML::Node pair;
      for (size_t camera = 0; camera < 2; ++camera) {
        const fs::path png = staging.path / name(index, camera, ".png");
        const fs::path jpg = staging.path / name(index, camera, ".jpg");
        HM_RETURN_IF_ERROR(snapshot_regular_file_for_rollback(images[index][camera], png, false, kMaximumImageBytes));
        YAML::Node identity;
        HM_ASSIGN_OR_RETURN(identity, file_identity(png, kMaximumImageBytes, true));
        if (saved && identity["sha256"].as<std::string>() != saved->image_digests.at(index)[camera])
          return absl::AbortedError("Saved source frame changed while its bundle was copied");
        if (saved) {
          // Preserve the original diagnostic bytes, including across encoder
          // versions. Copying a calibration must not regenerate its images.
          HM_RETURN_IF_ERROR(snapshot_regular_file_for_rollback(
              saved->thumbnails.at(index)[camera], jpg, false, kMaximumThumbnailBytes));
        } else {
          // CPU PNGs are already required for matching. This bounded diagnostic
          // reuses them; it never maps another video surface or uploads to the GPU.
          const cv::Mat image = cv::imread(png.string(), cv::IMREAD_UNCHANGED);
          if (image.empty())
            return absl::FailedPreconditionError("Cannot decode saved calibration PNG");
          cv::Mat thumbnail;
          const double scale = std::min(1.0, 1024.0 / std::max(image.cols, image.rows));
          cv::resize(image, thumbnail, {}, scale, scale, cv::INTER_AREA);
          if (thumbnail.depth() == CV_16U)
            thumbnail.convertTo(thumbnail, CV_8U, 255.0 / 65535.0);
          if (!cv::imwrite(jpg.string(), thumbnail, {cv::IMWRITE_JPEG_QUALITY, 88}))
            return absl::InternalError("Cannot write saved frame thumbnail");
        }
        YAML::Node thumbnail_identity;
        HM_ASSIGN_OR_RETURN(thumbnail_identity, file_identity(jpg, kMaximumThumbnailBytes, false));
        if (saved && thumbnail_identity["sha256"].as<std::string>() != saved->thumbnail_digests.at(index)[camera])
          return absl::AbortedError("Saved source thumbnail changed while its bundle was copied");
        pair[camera == 0 ? "left" : "right"]["image"] = identity;
        pair[camera == 0 ? "left" : "right"]["thumbnail"] = thumbnail_identity;
        HM_RETURN_IF_ERROR(fsync_stitch_path(png));
        HM_RETURN_IF_ERROR(fsync_stitch_path(jpg));
      }
      manifest["pairs"].push_back(pair);
    }
    const std::string contents = YAML::Dump(manifest) + "\n";
    if (contents.size() > kMaximumManifestBytes)
      return absl::ResourceExhaustedError("Saved frame manifest exceeds its size limit");
    HM_RETURN_IF_ERROR(write_stitch_transaction_file(staging.path / "frames.yaml", contents));
    HM_RETURN_IF_ERROR(fsync_stitch_path(staging.path, true));
    fs::rename(staging.path, pinned->path() / plan.fingerprint, error);
    if (error)
      return absl::InternalError("Cannot publish saved frame bundle: " + error.message());
    HM_RETURN_IF_ERROR(fsync_stitch_path(pinned->path(), true));
    return fsync_stitch_path(game_directory, true);
  } catch (const cv::Exception& exception) {
    return absl::InternalError("Cannot encode saved calibration frames: " + std::string(exception.what()));
  } catch (const YAML::Exception& exception) {
    return absl::InternalError("Cannot write saved frame manifest: " + std::string(exception.what()));
  }
}

absl::Status PublishPlayerFrameInputs(
    const fs::path& game_directory,
    const PlayerFrameSelectionPlan& plan,
    const std::vector<std::array<fs::path, 2>>& images) {
  return publish_player_frame_inputs(game_directory, plan, images, nullptr);
}

absl::Status CopyPlayerFrameInputs(
    const fs::path& source_game_directory,
    const fs::path& destination_game_directory,
    const PlayerFrameSelectionPlan& plan) {
  auto source = LoadPlayerFrameInputs(source_game_directory, plan);
  if (!source.ok())
    return source.status();
  if (!source->has_value())
    return absl::NotFoundError("Selected calibration input bundle is missing");
  return publish_player_frame_inputs(destination_game_directory, plan, (**source).images, &**source);
}
} // namespace hm::stitching
