#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/libs/stitching/CalibrationFrameExif.h"

namespace hm::stitching {

struct RinkMaskFrameProvenance {
  std::string selection{"auto"};
  std::optional<uint64_t> recording_time_ns;
  std::array<CalibrationFrameSource, 2> sources;
  // The transaction adopts only this small reference. Payloads never enter
  // rink rollback snapshots; their original source paths remain historical.
  std::string input_id;
};

struct RinkMaskInputLimits {
  uint64_t max_raw_bytes;
  uint64_t max_file_bytes;
};

// Reads required baseline/overlay values; no compiled-in default limits.
absl::StatusOr<RinkMaskInputLimits> ReadRinkMaskInputLimits(const YAML::Node& effective_config);

struct RinkMaskInputDescription {
  uint32_t width{0};
  uint32_t height{0};
  // rgba8, or half4-bits (four exact uint16 half encodings, never quantized).
  std::string format;
  std::string hugin_generation;
  std::string hugin_fingerprint;
  RinkMaskFrameProvenance frame;
};

struct RinkMaskInput {
  std::string id;
  RinkMaskInputDescription description;
  uint64_t file_bytes{0};
  // Keeps the validated payload inode open and leased against pruning.
  struct Storage;
  std::shared_ptr<Storage> storage;
};

using RinkMaskInputReadRow = std::function<absl::Status(uint32_t row, void* pixels, size_t bytes)>;
using RinkMaskInputWriteRow = std::function<absl::Status(uint32_t row, const void* pixels, size_t bytes)>;

// Publish does not change config.yaml. Hold its returned lease until the rink
// transaction adopts frame.input_id. Cancellation/failure leaves the old reference intact.
absl::StatusOr<RinkMaskInput> PublishRinkMaskInput(
    const std::filesystem::path& game_directory,
    const RinkMaskInputDescription& description,
    const RinkMaskInputLimits& limits,
    const RinkMaskInputReadRow& read_row,
    const std::function<bool()>& is_cancelled = {});

// Manifest and encoded bytes are validated before any consumer sees pixels.
// Absence returns NotFound; corrupt or stale evidence must never be substituted
// by a playback frame. Hugin identity/frame selection are checked by callers.
absl::StatusOr<RinkMaskInput> InspectRinkMaskInput(
    const std::filesystem::path& game_directory,
    const std::string& id,
    const RinkMaskInputLimits& limits,
    const std::function<bool()>& is_cancelled = {});
// Resolves the adopted reference and checks its selected-time provenance and
// loaded mapping generation. Width/height zero omit the optional geometry check.
absl::StatusOr<RinkMaskInput> LoadRinkMaskInputForConfig(
    const std::filesystem::path& game_directory,
    const YAML::Node& config,
    const std::string& hugin_generation,
    const std::string& hugin_fingerprint,
    uint32_t width = 0,
    uint32_t height = 0,
    const std::function<bool()>& is_cancelled = {});
absl::Status ReadRinkMaskInputRows(
    const RinkMaskInput& input,
    const RinkMaskInputWriteRow& write_row,
    const std::function<bool()>& is_cancelled = {});

// Copies exact manifest/payload bytes; paths/inodes are not provenance bindings.
absl::Status CopyRinkMaskInput(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::string& id,
    const RinkMaskInputLimits& limits);
absl::StatusOr<std::vector<std::filesystem::path>> RinkMaskInputArtifacts(const std::string& id);
// Call only after durable reference adoption, while holding artifact/config locks.
// Bundles leased by another in-flight producer/reader are retained.
absl::Status PruneRinkMaskInputs(const std::filesystem::path& game_directory, const std::string& keep_id);

} // namespace hm::stitching
