#include "hstream/src/libs/draw_display/AnalyticsOverlayAtlas.h"
#include "hstream/src/libs/draw_display/AnalyticsOverlayInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace hm::draw_display::analytics {
namespace {
using detail::Command;
using detail::Kind;
using detail::Tile;
constexpr uint32_t kMaximumSide = 16384;
constexpr uint32_t kMaximumTileCandidates = 1024 * 1024;
constexpr size_t kSlotBytes = kMaximumUploadBytes;
constexpr uint32_t kHashCapacity = kMaximumActiveTiles * 2;
constexpr uint32_t kNoReference = std::numeric_limits<uint32_t>::max();

bool Finite(float x) {
  return std::isfinite(x) && std::abs(x) <= 1000000.0F;
}
bool ValidColor(Color c) {
  return std::isfinite(c.red) && std::isfinite(c.green) && std::isfinite(c.blue) && std::isfinite(c.alpha) &&
      c.red >= 0 && c.red <= 1 && c.green >= 0 && c.green <= 1 && c.blue >= 0 && c.blue <= 1 && c.alpha >= 0 &&
      c.alpha <= 1;
}
bool ValidCommand(const Command& c) {
  return Finite(c.x0) && Finite(c.y0) && Finite(c.x1) && Finite(c.y1) && std::isfinite(c.radius) && c.radius >= 0 &&
      c.radius <= kMaximumSide && ValidColor(c.color);
}
size_t Align(size_t bytes) {
  return (bytes + 15U) & ~size_t(15U);
}

struct Bounds {
  float left, top, right, bottom;
};
Bounds CommandBounds(const Command& c) {
  if (c.kind == Kind::kLine)
    return {
        std::min(c.x0, c.x1) - c.radius - 1,
        std::min(c.y0, c.y1) - c.radius - 1,
        std::max(c.x0, c.x1) + c.radius + 1,
        std::max(c.y0, c.y1) + c.radius + 1};
  if (c.kind == Kind::kDisc)
    return {c.x0 - c.radius - 1, c.y0 - c.radius - 1, c.x0 + c.radius + 1, c.y0 + c.radius + 1};
  return {c.x0, c.y0, c.x1, c.y1};
}

// Conservative intersection eliminates empty centers of rectangle outlines and
// most empty tiles in long diagonal-line AABBs. Pixel coverage stays in CUDA.
bool TouchesTile(const Command& c, uint32_t tx, uint32_t ty) {
  const float left = tx * kTileSize, top = ty * kTileSize;
  const float right = left + kTileSize, bottom = top + kTileSize;
  if (c.kind == Kind::kRectangle) {
    return !(
        left >= c.x0 + c.radius && right <= c.x1 - c.radius && top >= c.y0 + c.radius && bottom <= c.y1 - c.radius);
  }
  if (c.kind == Kind::kLine) {
    // Segment versus the tile expanded by the round line radius. Liang-Barsky
    // slab clipping, inclusive so antialias boundary pixels are never omitted.
    const float radius = c.radius + 1;
    const float p[2] = {c.x0, c.y0}, d[2] = {c.x1 - c.x0, c.y1 - c.y0};
    const float low[2] = {left - radius, top - radius}, high[2] = {right + radius, bottom + radius};
    float enter = 0, leave = 1;
    for (int axis = 0; axis < 2; ++axis) {
      if (d[axis] == 0) {
        if (p[axis] < low[axis] || p[axis] > high[axis])
          return false;
      } else {
        float a = (low[axis] - p[axis]) / d[axis], b = (high[axis] - p[axis]) / d[axis];
        if (a > b)
          std::swap(a, b);
        enter = std::max(enter, a);
        leave = std::min(leave, b);
        if (enter > leave)
          return false;
      }
    }
  }
  return true;
}

} // namespace

CommandList::CommandList(uint32_t capacity, uint32_t glyph_capacity)
    : capacity_(std::min(capacity, kMaximumCommands)), glyph_capacity_(std::min(glyph_capacity, kMaximumGlyphs)) {}
void CommandList::Clear() noexcept {
  commands_.clear();
  glyphs_ = 0;
  rejected_ = 0;
}
bool CommandList::Add(Command command) {
  if (!ValidCommand(command)) {
    ++rejected_;
    return false;
  }
  if (command.color.alpha == 0)
    return true;
  if (commands_.size() >= capacity_) {
    ++rejected_;
    return false;
  }
  try {
    if (commands_.capacity() < capacity_)
      commands_.reserve(capacity_);
    commands_.push_back(command);
  } catch (const std::bad_alloc&) {
    ++rejected_;
    return false;
  }
  return true;
}
bool CommandList::AddLine(float x0, float y0, float x1, float y1, float width, Color color) {
  if (!(width > 0)) {
    ++rejected_;
    return false;
  }
  return Add({x0, y0, x1, y1, color, width * 0.5F, Kind::kLine, 0, 0});
}
bool CommandList::AddDisc(float x, float y, float radius, Color color) {
  if (!(radius > 0)) {
    ++rejected_;
    return false;
  }
  return Add({x, y, x, y, color, radius, Kind::kDisc, 0, 0});
}
bool CommandList::AddRectangle(float left, float top, float right, float bottom, float width, Color color) {
  if (!(width > 0 && right > left && bottom > top)) {
    ++rejected_;
    return false;
  }
  return Add({left, top, right, bottom, color, width, Kind::kRectangle, 0, 0});
}
bool CommandList::AddFill(float left, float top, float right, float bottom, Color color) {
  if (!(right > left && bottom > top)) {
    ++rejected_;
    return false;
  }
  return Add({left, top, right, bottom, color, 0, Kind::kFill, 0, 0});
}
bool CommandList::AddText(float x, float y, float height, std::string_view text, Color color) {
  const float advance = height * (static_cast<float>(detail::kGlyphWidth) / detail::kGlyphHeight);
  if (!(height > 0 && height <= 512) || text.size() > 64 || !Finite(x) || !Finite(y) || !ValidColor(color) ||
      !Finite(x + text.size() * advance) || !Finite(y + height)) {
    ++rejected_;
    return false;
  }
  uint32_t glyphs = 0;
  for (unsigned char c : text) {
    if (c < detail::kFirstGlyph || c >= detail::kFirstGlyph + detail::kGlyphCount) {
      ++rejected_;
      return false;
    }
    glyphs += c != ' ';
  }
  if (color.alpha == 0 || glyphs == 0)
    return true;
  if (glyphs > capacity_ - commands_.size() || glyphs > glyph_capacity_ - glyphs_) {
    ++rejected_;
    return false;
  }
  try {
    if (commands_.capacity() < capacity_)
      commands_.reserve(capacity_);
  } catch (const std::bad_alloc&) {
    ++rejected_;
    return false;
  }
  for (unsigned char c : text) {
    if (c != ' ')
      commands_.push_back(
          {x, y, x + advance, y + height, color, 0, Kind::kGlyph, static_cast<uint32_t>(c - detail::kFirstGlyph), 0});
    x += advance;
  }
  glyphs_ += glyphs;
  return true;
}

struct Compositor::State {
  struct Reference {
    uint32_t command, next;
  };
  struct Bin {
    uint32_t tile, first, last, count;
  };
  struct HashEntry {
    uint32_t generation{0}, tile{0}, bin{0};
  };
  struct Slot {
    uint8_t* host{nullptr};
    uint8_t* device{nullptr};
    cudaEvent_t complete{nullptr};
    bool pending{false};
  };
  std::array<Slot, 3> slots;
  std::vector<Reference> references;
  std::vector<Bin> bins;
  std::vector<HashEntry> hash;
  uint32_t generation{0};
  uint8_t* atlas_host{nullptr};
  uint8_t* atlas_device{nullptr};
  bool atlas_built{false};
  bool atlas_ready{false};
  bool font_failed{false};
  bool bound{false};
  bool poisoned{false};
  int device{-1};
  cudaStream_t stream{nullptr};
  size_t next_slot{0};

  ~State() {
    for (auto& slot : slots) {
      if (slot.complete)
        cudaEventDestroy(slot.complete);
      if (slot.device)
        cudaFree(slot.device);
      if (slot.host)
        cudaFreeHost(slot.host);
    }
    if (atlas_device)
      cudaFree(atlas_device);
    if (atlas_host)
      cudaFreeHost(atlas_host);
  }
};

Compositor::Compositor(Limits limits, const char* font_path)
    : limits_(limits), font_path_(font_path ? font_path : "") {}
Compositor::~Compositor() = default;

RenderResult Compositor::Render(
    const ImageView& image,
    const CommandList& commands,
    cudaStream_t stream,
    const TimingEvents* timing) {
  ++counters_.renders;
  if (commands.empty()) {
    ++counters_.empty_renders;
    return {};
  }
  RenderResult result;
  auto invalid = [&] {
    ++counters_.invalid_requests;
    result.status = RenderStatus::kInvalidArgument;
    return result;
  };
  auto capacity = [&] {
    ++counters_.capacity_suppressions;
    result.status = RenderStatus::kCapacity;
    return result;
  };
  auto cuda_failure = [&](cudaError_t error) {
    // A failed asynchronous API may also report an earlier stream error. Do
    // not reuse potentially live pinned storage after any CUDA error.
    if (state_)
      state_->poisoned = true;
    result.status = RenderStatus::kCudaError;
    result.cuda_error = error;
    return result;
  };
  if (!image.data || !image.width || !image.height || image.width > kMaximumSide || image.height > kMaximumSide ||
      image.pitch < size_t(image.width) * 4 || image.pitch % 4 || reinterpret_cast<uintptr_t>(image.data) % 4 ||
      image.pitch > std::numeric_limits<size_t>::max() / image.height ||
      (image.format != PixelFormat::kRgba8 && image.format != PixelFormat::kRgb10A2) || !limits_.active_tiles ||
      limits_.active_tiles > kMaximumActiveTiles || !limits_.tile_references ||
      limits_.tile_references > kMaximumTileReferences || !limits_.upload_bytes ||
      limits_.upload_bytes > kMaximumUploadBytes)
    return invalid();
  try {
    if (!state_)
      state_ = std::make_unique<State>();
    State& state = *state_;
    if (state.poisoned)
      return cuda_failure(cudaErrorUnknown);
    if (state.bound && state.stream != stream)
      return invalid();
    state.references.clear();
    state.bins.clear();
    if (state.references.capacity() < limits_.tile_references)
      state.references.reserve(limits_.tile_references);
    if (state.bins.capacity() < limits_.active_tiles)
      state.bins.reserve(limits_.active_tiles);
    if (state.hash.empty())
      state.hash.resize(kHashCapacity);
    if (++state.generation == 0) {
      std::fill(state.hash.begin(), state.hash.end(), State::HashEntry{});
      ++state.generation;
    }
    const uint32_t columns = (image.width + kTileSize - 1) / kTileSize;
    uint32_t candidates = 0;
    bool glyphs = false;
    for (uint32_t ci = 0; ci < commands.size(); ++ci) {
      const Command& command = commands.data()[ci];
      const Bounds b = CommandBounds(command);
      const float left = std::max(0.0F, b.left), top = std::max(0.0F, b.top);
      const float right = std::min(static_cast<float>(image.width), b.right);
      const float bottom = std::min(static_cast<float>(image.height), b.bottom);
      if (!(left < right && top < bottom))
        continue;
      const uint32_t x0 = static_cast<uint32_t>(std::floor(left)) / kTileSize;
      const uint32_t y0 = static_cast<uint32_t>(std::floor(top)) / kTileSize;
      const uint32_t x1 = (static_cast<uint32_t>(std::ceil(right)) - 1) / kTileSize;
      const uint32_t y1 = (static_cast<uint32_t>(std::ceil(bottom)) - 1) / kTileSize;
      const uint32_t count = (x1 - x0 + 1) * (y1 - y0 + 1);
      if (count > kMaximumTileCandidates - candidates)
        return capacity();
      candidates += count;
      for (uint32_t y = y0; y <= y1; ++y) {
        for (uint32_t x = x0; x <= x1; ++x) {
          if (!TouchesTile(command, x, y))
            continue;
          if (state.references.size() == limits_.tile_references)
            return capacity();
          const uint32_t tile = y * columns + x;
          uint32_t slot_index = (tile * 2654435761U) & (kHashCapacity - 1);
          while (state.hash[slot_index].generation == state.generation && state.hash[slot_index].tile != tile)
            slot_index = (slot_index + 1) & (kHashCapacity - 1);
          auto& entry = state.hash[slot_index];
          const uint32_t reference_index = state.references.size();
          if (entry.generation != state.generation) {
            if (state.bins.size() == limits_.active_tiles)
              return capacity();
            entry = {state.generation, tile, static_cast<uint32_t>(state.bins.size())};
            state.bins.push_back({tile, reference_index, reference_index, 0});
          }
          auto& bin = state.bins[entry.bin];
          if (bin.count)
            state.references[bin.last].next = reference_index;
          bin.last = reference_index;
          ++bin.count;
          state.references.push_back({ci, kNoReference});
          glyphs |= command.kind == Kind::kGlyph;
        }
      }
    }
    if (state.references.empty())
      return result;
    const size_t tile_offset = Align(commands.size() * sizeof(Command));
    const size_t ref_offset = Align(tile_offset + state.bins.size() * sizeof(Tile));
    const size_t total_bytes = ref_offset + state.references.size() * sizeof(uint32_t);
    if (total_bytes > limits_.upload_bytes)
      return capacity();

    int device;
    cudaError_t error = cudaGetDevice(&device);
    if (error != cudaSuccess)
      return cuda_failure(error);
    if (state.bound && state.device != device)
      return invalid();
    if (!state.bound) {
      state.bound = true;
      state.device = device;
      state.stream = stream;
    }
    State::Slot* slot = nullptr;
    for (size_t i = 0; i < state.slots.size(); ++i) {
      const size_t index = (state.next_slot + i) % state.slots.size();
      auto& candidate = state.slots[index];
      if (candidate.pending) {
        error = cudaEventQuery(candidate.complete);
        if (error == cudaErrorNotReady)
          continue;
        if (error != cudaSuccess)
          return cuda_failure(error);
        candidate.pending = false;
      }
      slot = &candidate;
      state.next_slot = (index + 1) % state.slots.size();
      break;
    }
    if (!slot) {
      ++counters_.busy_suppressions;
      result.status = RenderStatus::kBusy;
      return result;
    }
    if (!slot->host) {
      error = cudaHostAlloc(reinterpret_cast<void**>(&slot->host), kSlotBytes, cudaHostAllocDefault);
      if (error != cudaSuccess)
        return cuda_failure(error);
      ++counters_.host_allocations;
      counters_.pinned_host_bytes += kSlotBytes;
    }
    if (!slot->device) {
      error = cudaMalloc(reinterpret_cast<void**>(&slot->device), kSlotBytes);
      if (error != cudaSuccess)
        return cuda_failure(error);
      ++counters_.device_allocations;
      counters_.device_bytes += kSlotBytes;
    }
    if (!slot->complete) {
      error = cudaEventCreateWithFlags(&slot->complete, cudaEventDisableTiming);
      if (error != cudaSuccess)
        return cuda_failure(error);
      ++counters_.event_creations;
    }
    if (glyphs && !state.atlas_ready) {
      if (state.font_failed) {
        result.status = RenderStatus::kFontUnavailable;
        return result;
      }
      if (!state.atlas_host) {
        error = cudaHostAlloc(reinterpret_cast<void**>(&state.atlas_host), detail::kAtlasBytes, cudaHostAllocDefault);
        if (error != cudaSuccess)
          return cuda_failure(error);
        ++counters_.host_allocations;
        counters_.pinned_host_bytes += detail::kAtlasBytes;
      }
      if (!state.atlas_built) {
        if (!detail::BuildAtlas(state.atlas_host, font_path_)) {
          state.font_failed = true;
          result.status = RenderStatus::kFontUnavailable;
          return result;
        }
        state.atlas_built = true;
      }
      if (!state.atlas_device) {
        error = cudaMalloc(reinterpret_cast<void**>(&state.atlas_device), detail::kAtlasBytes);
        if (error != cudaSuccess)
          return cuda_failure(error);
        ++counters_.device_allocations;
        counters_.device_bytes += detail::kAtlasBytes;
      }
      error =
          cudaMemcpyAsync(state.atlas_device, state.atlas_host, detail::kAtlasBytes, cudaMemcpyHostToDevice, stream);
      if (error != cudaSuccess)
        return cuda_failure(error);
      ++counters_.h2d_calls;
      counters_.h2d_bytes += detail::kAtlasBytes;
      counters_.glyph_upload_bytes += detail::kAtlasBytes;
      result.glyph_upload_bytes = detail::kAtlasBytes;
      state.atlas_ready = true;
    }
    std::memcpy(slot->host, commands.data(), commands.size() * sizeof(Command));
    auto* tiles = reinterpret_cast<Tile*>(slot->host + tile_offset);
    auto* refs = reinterpret_cast<uint32_t*>(slot->host + ref_offset);
    uint32_t reference_index = 0;
    for (size_t i = 0; i < state.bins.size(); ++i) {
      const auto& bin = state.bins[i];
      tiles[i] = {bin.tile % columns, bin.tile / columns, reference_index, bin.count};
      // Commands entered every bin in input order. Flattening linked indices
      // preserves overlap order without an O(reference_count log N) frame sort.
      for (uint32_t r = bin.first; r != kNoReference; r = state.references[r].next)
        refs[reference_index++] = state.references[r].command;
    }
    if (timing && timing->before_upload) {
      error = cudaEventRecord(timing->before_upload, stream);
      if (error != cudaSuccess)
        return cuda_failure(error);
    }
    error = cudaMemcpyAsync(slot->device, slot->host, total_bytes, cudaMemcpyHostToDevice, stream);
    if (error != cudaSuccess)
      return cuda_failure(error);
    ++counters_.h2d_calls;
    counters_.h2d_bytes += total_bytes;
    result.commands = commands.size();
    result.active_tiles = state.bins.size();
    result.tile_references = state.references.size();
    result.upload_bytes = total_bytes;
    if (timing && timing->before_raster) {
      error = cudaEventRecord(timing->before_raster, stream);
      if (error != cudaSuccess) {
        state.poisoned = true;
        return cuda_failure(error);
      }
    }
    error = detail::Raster(
        image,
        reinterpret_cast<const Command*>(slot->device),
        reinterpret_cast<const Tile*>(slot->device + tile_offset),
        reinterpret_cast<const uint32_t*>(slot->device + ref_offset),
        state.bins.size(),
        state.atlas_device,
        stream);
    // Fence even failed launches: the asynchronous command upload may be live.
    const cudaError_t event_error = cudaEventRecord(slot->complete, stream);
    if (event_error != cudaSuccess) {
      state.poisoned = true;
      return cuda_failure(event_error);
    }
    slot->pending = true;
    if (error != cudaSuccess)
      return cuda_failure(error);
    ++counters_.raster_launches;
    result.launched = true;
    if (timing && timing->after_raster) {
      error = cudaEventRecord(timing->after_raster, stream);
      if (error != cudaSuccess)
        return cuda_failure(error);
    }
    return result;
  } catch (const std::bad_alloc&) {
    return capacity();
  }
}
} // namespace hm::draw_display::analytics
