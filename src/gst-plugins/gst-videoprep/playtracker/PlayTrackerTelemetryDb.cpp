#include "hstream/src/gst-plugins/gst-videoprep/playtracker/PlayTrackerTelemetryDb.h"
#include <fcntl.h>
#include <openssl/sha.h>
#include <unistd.h>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include "hstream/src/libs/recording/Database.h"

namespace hm::playtracker {
using hm::recording::Database;
using hm::recording::Statement;
namespace fs = std::filesystem;
namespace {
std::string hash(const std::string& data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
  std::ostringstream out;
  for (const auto byte : digest)
    out << std::hex << std::setw(2) << std::setfill('0') << int(byte);
  return out.str();
}
std::string now() {
  const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}
const char* outcome_name(TelemetryRunOutcome outcome) {
  switch (outcome) {
    case TelemetryRunOutcome::kEndOfStream:
      return "end-of-stream";
    case TelemetryRunOutcome::kIntentionalStop:
      return "intentional-stop";
    case TelemetryRunOutcome::kFailed:
      return "failed";
    default:
      return "incomplete";
  }
}
} // namespace
struct PlayTrackerTelemetryDb::Impl {
  struct SampleItem {
    uint64_t id;
    TelemetrySample sample;
    std::shared_ptr<const TelemetryGeometry> geometry;
  };
  struct EventItem {
    uint64_t boundary;
    TelemetryConfigEvent event;
  };
  using Item = std::variant<SampleItem, EventItem>;
  mutable std::mutex mutex;
  std::mutex producers;
  std::condition_variable ready, space;
  std::deque<Item> queue;
  size_t capacity{2048};
  bool active{false}, stopping{false}, failed{false};
  TelemetryRunOutcome outcome{TelemetryRunOutcome::kIncomplete};
  uint64_t high{0}, count{0}, event_id{0}, geometry_id{0};
  std::string path, run_id;
  std::thread thread;
  std::unique_ptr<Database> db;
  std::map<std::string, std::unique_ptr<Statement>> statements;
  std::shared_ptr<const TelemetryGeometry> last_geometry;
  uint32_t width{0}, height{0};
  Statement& query(const std::string& sql) {
    auto& statement = statements[sql];
    if (!statement)
      statement = std::make_unique<Statement>(db->get(), sql);
    return *statement;
  }
  bool admit(std::unique_lock<std::mutex>& lock) {
    space.wait(lock, [&] { return !active || stopping || queue.size() < capacity; });
    return active && !stopping;
  }
  void event(const EventItem& item) {
    auto& q = query("INSERT INTO config_events VALUES(?,?,?,?,?,?,?,?)");
    q.Bind(1, run_id);
    q.Bind(2, ++event_id);
    q.Bind(3, item.boundary);
    q.Bind(4, item.event.kind);
    q.Bind(5, item.event.key);
    q.Bind(6, item.event.value);
    q.Bind(7, item.event.artifact_stem);
    q.Bind(8, item.event.artifact_contents);
    q.Execute();
  }
  void sample(const SampleItem& item) {
    const auto& s = item.sample;
    if (!s.width || !s.height || s.width > 32768 || s.height > 32768)
      throw std::runtime_error("Invalid recording canvas dimensions");
    if (!geometry_id || width != s.width || height != s.height || last_geometry != item.geometry) {
      const auto& g = item.geometry;
      if (g && (g->width != s.width || g->height != s.height))
        throw std::runtime_error("Rink mask dimensions differ from the tracking canvas");
      const std::string mask = g && g->encode_mask ? g->encode_mask() : std::string();
      if (g && g->encode_mask && mask.empty())
        throw std::runtime_error("Rink mask encoding returned no data");
      auto& q = query("INSERT INTO geometries VALUES(?,?,?,?,?,?,?,?,?,?)");
      q.Bind(1, run_id);
      q.Bind(2, ++geometry_id);
      q.Bind(3, s.width);
      q.Bind(4, s.height);
      q.Bind(5, "original_stitched_pixels");
      q.Bind(6, g ? g->revision : "unavailable");
      if (mask.empty()) {
        q.Null(7);
        q.Null(8);
        q.Null(9);
      } else {
        q.Bind(7, "png");
        q.Blob(8, mask);
        q.Bind(9, hash(mask));
      }
      q.Bind(10, "[[1,0,0],[0,1,0]]");
      q.Execute();
      last_geometry = g;
      width = s.width;
      height = s.height;
    }
    auto& f = query("INSERT INTO frames VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
    f.Bind(1, run_id);
    f.Bind(2, item.id);
    f.Bind(3, s.source_id);
    f.Bind(4, s.source_frame);
    f.Bind(5, s.decoded_source_id);
    f.Bind(6, s.decoded_sequence);
    f.Bind(7, s.pts_ns);
    f.Bind(8, s.ntp_ns);
    f.Bind(9, s.seek_epoch);
    f.Bind(10, s.replay ? s.replay->reset_epoch : uint64_t(0));
    f.Bind(11, geometry_id);
    f.Bind(12, uint64_t(s.detections.size()));
    f.Bind(13, uint64_t(s.tracks.size()));
    f.Execute();
    for (size_t i = 0; i < s.detections.size(); ++i) {
      const auto& b = s.detections[i];
      auto& q = query("INSERT INTO detections VALUES(?,?,?,?,?,?,?,?,?)");
      q.Bind(1, run_id);
      q.Bind(2, item.id);
      q.Bind(3, uint64_t(i));
      q.Bind(4, double(b.left));
      q.Bind(5, double(b.top));
      q.Bind(6, double(b.width));
      q.Bind(7, double(b.height));
      q.Bind(8, double(b.score));
      q.Bind(9, b.class_id);
      q.Execute();
    }
    for (size_t i = 0; i < s.tracks.size(); ++i) {
      const auto& b = s.tracks[i];
      auto& q = query("INSERT INTO tracks VALUES(?,?,?,?,?,?,?,?,?,?,?)");
      q.Bind(1, run_id);
      q.Bind(2, item.id);
      q.Bind(3, uint64_t(i));
      q.Bind(4, std::to_string(b.tracking_id));
      q.Bind(5, double(b.left));
      q.Bind(6, double(b.top));
      q.Bind(7, double(b.width));
      q.Bind(8, double(b.height));
      q.Bind(9, double(b.score));
      q.Bind(10, b.class_id);
      q.Bind(11, "{}");
      q.Execute();
    }
    if (!s.policy_boxes.empty()) {
      for (const auto* role : {"fast", "program"}) {
        const auto& b = std::string(role) == "fast" ? s.policy_boxes.front() : s.policy_boxes.back();
        auto& q = query("INSERT INTO cameras VALUES(?,?,?,?,?,?,?)");
        q.Bind(1, run_id);
        q.Bind(2, item.id);
        q.Bind(3, role);
        q.Bind(4, double(b.left));
        q.Bind(5, double(b.top));
        q.Bind(6, double(b.width));
        q.Bind(7, double(b.height));
        q.Execute();
      }
    }
    if (s.replay) {
      const auto& r = *s.replay;
      auto& q = query("INSERT INTO replay_frames VALUES(?,?,?,?,?,?,?,?,?,?)");
      q.Bind(1, run_id);
      q.Bind(2, item.id);
      for (int i = 0; i < 4; ++i)
        q.Bind(i + 3, double(r.arena[i]));
      q.Bind(7, int(r.stepped));
      q.Bind(8, int(r.has_received_tracks));
      q.Bind(9, double(r.edge_rotation_left));
      q.Bind(10, double(r.edge_rotation_right));
      q.Execute();
      for (size_t i = 0; i < r.tracks.size(); ++i) {
        auto& t = query("INSERT INTO replay_tracks VALUES(?,?,?,?,?,?,?,?)");
        t.Bind(1, run_id);
        t.Bind(2, item.id);
        t.Bind(3, uint64_t(i));
        t.Bind(4, std::to_string(r.tracks[i].first));
        for (int c = 0; c < 4; ++c)
          t.Bind(c + 5, double(r.tracks[i].second[c]));
        t.Execute();
      }
      if (!r.checkpoint.empty()) {
        if (r.base_checkpoint.empty())
          throw std::runtime_error("Missing base checkpoint");
        auto& c = query("INSERT INTO checkpoints VALUES(?,?,?,?,?)");
        c.Bind(1, run_id);
        c.Bind(2, item.id);
        c.Bind(3, 1);
        c.Bind(4, r.checkpoint);
        c.Bind(5, r.base_checkpoint);
        c.Execute();
      }
    }
    ++count;
  }
  void writer() {
    try {
      db->Exec("BEGIN IMMEDIATE");
      size_t batch = 0;
      for (;;) {
        Item item;
        {
          std::unique_lock<std::mutex> lock(mutex);
          ready.wait(lock, [&] { return stopping || !queue.empty(); });
          if (queue.empty())
            break;
          item = std::move(queue.front());
          queue.pop_front();
        }
        space.notify_one();
        if (const auto* s = std::get_if<SampleItem>(&item))
          sample(*s);
        else
          event(std::get<EventItem>(item));
        if (++batch == 120) {
          db->Exec("COMMIT; BEGIN IMMEDIATE");
          batch = 0;
        }
      }
      db->Exec("COMMIT");
    } catch (const std::exception& e) {
      std::cerr << "Telemetry database writer failed: " << e.what() << std::endl;
      sqlite3_exec(db->get(), "ROLLBACK", nullptr, nullptr, nullptr);
      std::lock_guard<std::mutex> lock(mutex);
      failed = true;
      active = false;
      queue.clear();
      space.notify_all();
    }
  }
};
PlayTrackerTelemetryDb::PlayTrackerTelemetryDb() : impl_(std::make_unique<Impl>()) {}
PlayTrackerTelemetryDb::~PlayTrackerTelemetryDb() {
  Stop();
}
absl::Status PlayTrackerTelemetryDb::Start(
    const std::string& directory,
    TelemetryConfigArtifact source,
    TelemetryConfigArtifact effective,
    std::vector<TelemetryConfigEvent> startup,
    size_t capacity,
    const std::string& game_id) {
  Stop();
  impl_ = std::make_unique<Impl>();
  auto& p = *impl_;
  try {
    if (directory.empty() || !capacity || source.contents.empty() || effective.contents.empty())
      return absl::InvalidArgumentError("Telemetry requires a directory, queue capacity and configuration snapshots");
    fs::create_directories(directory);
    p.run_id = hm::recording::NewGuid();
    p.capacity = capacity;
    uint64_t first_generation = 0;
    const std::regex pattern(R"(^hstream_telemetry(?:-([0-9]+))?\.(?:db|json)$)");
    for (const auto& entry : fs::directory_iterator(directory)) {
      std::smatch match;
      const std::string filename = entry.path().filename().string();
      if (std::regex_match(filename, match, pattern)) {
        const uint64_t previous = match[1].matched ? std::stoull(match[1]) : 0;
        if (previous == UINT64_MAX)
          throw std::runtime_error("Recording generation exceeds integer range");
        first_generation = std::max(first_generation, previous + 1);
      }
    }
    for (uint64_t generation = first_generation;; ++generation) {
      p.path =
          (fs::path(directory) / ("hstream_telemetry" + (generation ? "-" + std::to_string(generation) : "") + ".db"))
              .string();
      int fd = open(p.path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
      if (fd >= 0) {
        close(fd);
        break;
      }
      if (errno != EEXIST)
        throw std::runtime_error("Cannot reserve telemetry database: " + p.path);
    }
    p.db = std::make_unique<Database>(p.path, true);
    p.db->Exec("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
    p.db->Exec(hm::recording::Schema());
    Statement run(
        p.db->get(),
        "INSERT INTO runs(run_id,game_id,started_utc,producer,source_config,effective_config) VALUES(?,?,?,?,?,?)");
    run.Bind(1, p.run_id);
    run.Bind(2, game_id.empty() ? fs::path(directory).filename().string() : game_id);
    run.Bind(3, now());
    run.Bind(4, "hstream");
    run.Bind(5, source.contents);
    run.Bind(6, effective.contents);
    run.Execute();
    p.event({1, {"base-config", "source", source.path, "play_tracker_source", source.contents}});
    for (auto& e : startup)
      p.event({1, std::move(e)});
    const int dir = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0)
      throw std::runtime_error("Cannot synchronize recording directory");
    const int synced = fsync(dir);
    close(dir);
    if (synced)
      throw std::runtime_error("Cannot synchronize recording directory");
    p.active = true;
    p.thread = std::thread([&p] { p.writer(); });
    return absl::OkStatus();
  } catch (const std::exception& e) {
    p.active = false;
    p.failed = true;
    return absl::InternalError(e.what());
  }
}
void PlayTrackerTelemetryDb::Stop() {
  auto& p = *impl_;
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    p.stopping = true;
  }
  p.ready.notify_all();
  p.space.notify_all();
  if (p.thread.joinable())
    p.thread.join();
  if (p.db) {
    try {
      Statement q(p.db->get(), "UPDATE runs SET completed=?,outcome=?,sample_count=? WHERE run_id=?");
      q.Bind(
          1,
          int(!p.failed && p.count &&
              (p.outcome == TelemetryRunOutcome::kEndOfStream || p.outcome == TelemetryRunOutcome::kIntentionalStop)));
      q.Bind(2, p.failed ? "failed" : outcome_name(p.outcome));
      q.Bind(3, p.count);
      q.Bind(4, p.run_id);
      q.Execute();
      p.statements.clear();
      p.db.reset();
    } catch (const std::exception& e) {
      p.failed = true;
      std::cerr << "Telemetry finalization failed: " << e.what() << std::endl;
    }
  }
  std::lock_guard<std::mutex> lock(p.mutex);
  p.active = false;
}
void PlayTrackerTelemetryDb::MarkRunOutcome(TelemetryRunOutcome outcome) {
  auto& p = *impl_;
  std::lock_guard<std::mutex> lock(p.mutex);
  if (p.outcome == TelemetryRunOutcome::kFailed)
    return;
  p.outcome = outcome;
  // A downstream failure may arrive after the writer has drained.
  if (!p.db && !p.path.empty() && outcome == TelemetryRunOutcome::kFailed) {
    try {
      Database db(p.path, true);
      db.Exec("UPDATE runs SET completed=0,outcome='failed'");
    } catch (const std::exception& e) {
      std::cerr << "Cannot invalidate telemetry: " << e.what() << std::endl;
    }
  }
}
bool PlayTrackerTelemetryDb::failed() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->failed;
}
bool PlayTrackerTelemetryDb::active() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->active;
}
std::string PlayTrackerTelemetryDb::output_manifest() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->path;
}
uint64_t PlayTrackerTelemetryDb::frame_id_high_watermark() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->high;
}
bool PlayTrackerTelemetryDb::TryEnqueue(TelemetrySample sample, std::shared_ptr<const TelemetryGeometry> geometry) {
  auto& p = *impl_;
  std::lock_guard<std::mutex> producer(p.producers);
  std::unique_lock<std::mutex> lock(p.mutex);
  if (!p.admit(lock))
    return false;
  p.queue.emplace_back(Impl::SampleItem{++p.high, std::move(sample), std::move(geometry)});
  lock.unlock();
  p.ready.notify_one();
  return true;
}
bool PlayTrackerTelemetryDb::TryRecordConfigEvent(TelemetryConfigEvent event) {
  auto& p = *impl_;
  std::lock_guard<std::mutex> producer(p.producers);
  std::unique_lock<std::mutex> lock(p.mutex);
  if (!p.admit(lock))
    return false;
  p.queue.emplace_back(Impl::EventItem{++p.high + 1, std::move(event)});
  lock.unlock();
  p.ready.notify_one();
  return true;
}
bool PlayTrackerTelemetryDb::TryRecordDiscontinuity(TelemetryConfigEvent event) {
  return TryRecordConfigEvent(std::move(event));
}
} // namespace hm::playtracker
