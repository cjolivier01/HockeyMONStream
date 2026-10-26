#include "hstream/src/libs/stitching/RinkMaskInputStore.h"

#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
using namespace hm::stitching;
namespace {
bool expect(bool ok, const char* message) {
  if (!ok)
    std::cerr << "FAIL: " << message << '\n';
  return ok;
}
} // namespace
int main() {
  char pattern[] = "/tmp/rink-mask-input-test-XXXXXX";
  const fs::path root = ::mkdtemp(pattern);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code e;
      fs::remove_all(path, e);
    }
  } cleanup{root};
  const fs::path game = root / "game", copy = root / "copy";
  fs::create_directory(game);
  fs::create_directory(copy);
  std::ofstream(root / "physical-left.mp4") << "left camera content";
  fs::create_symlink(root / "physical-left.mp4", game / "left.mp4");
  std::ofstream(game / "right.mp4") << "right camera content";
  const RinkMaskInputLimits limits{2147483648ULL, 2214592512ULL};
  RinkMaskInputDescription d;
  d.width = 7;
  d.height = 5;
  d.format = "half4-bits";
  d.hugin_generation = "logical-hugin-generation";
  d.hugin_fingerprint = std::string(64, 'a');
  d.frame.selection = "-00:00:01";
  d.frame.recording_time_ns = 12345678900ULL;
  d.frame.sources = {{{game / "left.mp4", 12.5}, {game / "right.mp4", 13.5}}};
  // Includes signed zeros, subnormals, infinities and NaN payloads. A conversion
  // to float or 8-bit would lose bits and fail this round-trip.
  const uint16_t values[] = {0, 0x8000, 1, 0x3c01, 0x7c00, 0xfc00, 0x7e73, 0xffff};
  std::vector<uint16_t> pixels(d.width * d.height * 4);
  for (size_t i = 0; i < pixels.size(); ++i)
    pixels[i] = values[i % 8];
  auto reader = [&](uint32_t y, void* row, size_t bytes) {
    std::memcpy(row, pixels.data() + y * d.width * 4, bytes);
    return absl::OkStatus();
  };
  auto published = PublishRinkMaskInput(game, d, limits, reader);
  if (!published.ok()) {
    std::cerr << published.status() << '\n';
    return 1;
  }
  bool ok = true;
  auto compare = [&](const RinkMaskInput& input) {
    return ReadRinkMaskInputRows(input, [&](uint32_t y, const void* row, size_t bytes) {
      return std::memcmp(row, pixels.data() + y * d.width * 4, bytes) == 0 ? absl::OkStatus()
                                                                           : absl::DataLossError("changed half bits");
    });
  };
  ok &= expect(compare(*published).ok(), "symlink source aliases capture and all half encodings round-trip exactly");
  ok &= expect(CopyRinkMaskInput(game, copy, published->id, limits).ok(), "copy preserves immutable evidence");
  YAML::Node selected;
  selected["stitching"]["rink_mask_input"]["max_raw_bytes"] = limits.max_raw_bytes;
  selected["stitching"]["rink_mask_input"]["max_file_bytes"] = limits.max_file_bytes;
  selected["stitching"]["rink_mask_frame_time"] = d.frame.selection;
  selected["rink"]["mask_frame"]["input_id"] = published->id;
  selected["rink"]["mask_frame"]["selection"] = d.frame.selection;
  selected["rink"]["mask_frame"]["recording_time_ns"] = *d.frame.recording_time_ns;
  for (const auto& source : d.frame.sources) {
    YAML::Node item;
    item["video"] = source.video.string();
    item["seconds"] = source.seconds;
    selected["rink"]["mask_frame"]["sources"].push_back(item);
  }
  int cancellation_checks = 0;
  auto stopped_load =
      LoadRinkMaskInputForConfig(game, selected, d.hugin_generation, d.hugin_fingerprint, d.width, d.height, [&] {
        return ++cancellation_checks > 1;
      });
  ok &= expect(
      absl::IsCancelled(stopped_load.status()) && cancellation_checks == 2,
      "active cancellation reaches the retained-input checksum loop");
  fs::remove(game / "left.mp4");
  fs::remove(game / "right.mp4");
  auto copied = InspectRinkMaskInput(copy, published->id, limits);
  ok &= expect(copied.ok() && compare(*copied).ok(), "new inodes and absent original source paths remain valid");
  ok &= expect(
      copied.ok() && copied->description.frame.sources[0].video == d.frame.sources[0].video &&
          copied->description.frame.recording_time_ns == d.frame.recording_time_ns,
      "copy preserves original provenance");
  ok &= expect(!InspectRinkMaskInput(copy, "../escape", limits).ok(), "invalid reference rejected");
  ok &= expect(!InspectRinkMaskInput(copy, published->id, {1, limits.max_file_bytes}).ok(), "raw byte bound enforced");
  ok &=
      expect(!InspectRinkMaskInput(copy, published->id, {limits.max_raw_bytes, 1}).ok(), "encoded byte bound enforced");
  ok &= expect(
      absl::IsCancelled(ReadRinkMaskInputRows(
          *published, [](uint32_t, const void*, size_t) { return absl::OkStatus(); }, [] { return true; })),
      "decode cancellation honored");
  ok &= expect(
      absl::IsCancelled(PublishRinkMaskInput(game, d, limits, reader, [] { return true; }).status()),
      "capture cancellation does not publish");
  auto oversized = d;
  oversized.width = 65537;
  ok &= expect(!PublishRinkMaskInput(game, oversized, limits, reader).ok(), "existing native geometry limit enforced");
  ok &= expect(
      PruneRinkMaskInputs(game, "").ok() && fs::exists(game / "rink-mask-input" / published->id),
      "in-flight lease protects a candidate from pruning");
  const std::string id = published->id;
  published = absl::CancelledError("release lease");
  ok &= expect(
      PruneRinkMaskInputs(game, "").ok() && !fs::exists(game / "rink-mask-input" / id),
      "unreferenced unleased evidence is pruned");
  // A same-size mutation must fail checksum validation before any row is consumed.
  const fs::path payload = copy / "rink-mask-input" / id / "input.tiff";
  {
    std::fstream file(payload, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(16);
    file.put('!');
  }
  ok &= expect(!InspectRinkMaskInput(copy, id, limits).ok(), "corrupt encoded bytes rejected");
  fs::resize_file(payload, 12);
  ok &= expect(!InspectRinkMaskInput(copy, id, limits).ok(), "truncated input rejected");
  fs::remove(payload);
  fs::create_symlink("/dev/zero", payload);
  ok &= expect(!InspectRinkMaskInput(copy, id, limits).ok(), "payload symlink rejected without following it");
  YAML::Node config;
  config["stitching"]["rink_mask_input"]["max_raw_bytes"] = limits.max_raw_bytes;
  config["stitching"]["rink_mask_input"]["max_file_bytes"] = limits.max_file_bytes;
  auto configured = ReadRinkMaskInputLimits(config);
  ok &=
      expect(configured.ok() && configured->max_raw_bytes == limits.max_raw_bytes, "explicit effective limits resolve");
  config["stitching"]["rink_mask_input"]["max_file_bytes"] = 0;
  ok &= expect(!ReadRinkMaskInputLimits(config).ok(), "invalid effective limit fails");
  return ok ? 0 : 1;
}
