#include "hstream/src/gst-plugins/gst-videoprep/playtracker/PlayTrackerTelemetryDb.h"
#include <unistd.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include "hstream/src/libs/recording/Database.h"

using namespace hm::playtracker;
using namespace hm::recording;
void check(bool condition, const std::string& error) {
  if (!condition)
    throw std::runtime_error(error);
}
int main(int argc, char** argv) {
  try {
    char tmp[] = "/tmp/hstream-db-test-XXXXXX";
    check(mkdtemp(tmp), "mkdtemp");
    const std::filesystem::path directory = argc > 1 ? argv[1] : tmp;
    PlayTrackerTelemetryDb writer;
    check(
        writer
            .Start(directory.string(), {"source", "source: true"}, {"effective", "effective: true"}, {}, 1, "game-one")
            .ok(),
        "start");
    const auto path = writer.output_manifest();
    {
      Database db(path);
      db.Validate();
      Statement q(db.get(), "SELECT completed FROM runs");
      check(q.Next() && !q.Int(0), "live database is incomplete");
    }
    auto geometry = std::make_shared<TelemetryGeometry>();
    geometry->width = 100;
    geometry->height = 50;
    geometry->revision = "revision-one";
    int encoded = 0;
    geometry->encode_mask = [&] {
      ++encoded;
      return std::string("compressed mask bytes");
    };
    for (uint64_t i = 0; i < 250; ++i) {
      TelemetrySample sample;
      sample.width = 100;
      sample.height = 50;
      sample.source_frame = i;
      sample.pts_ns = i * 10000000;
      sample.tracks.push_back({UINT64_MAX, 1, 2, 3, 4, 0.9f, 0});
      sample.detections.push_back({1, 2, 3, 4, 0.8f, 0});
      sample.policy_boxes.push_back({1, 2, 30, 40});
      if (i == 150)
        check(writer.TryRecordConfigEvent({"runtime-tuning", "key", "value", "name", "contents"}), "event");
      check(writer.TryEnqueue(std::move(sample), geometry), "sample");
    }
    writer.MarkRunOutcome(TelemetryRunOutcome::kIntentionalStop);
    writer.Stop();
    check(encoded == 1, "geometry is encoded only once on writer thread");
    std::string run;
    {
      Database db(path);
      db.Validate();
      Statement q(db.get(), "SELECT run_id,game_id,completed,sample_count FROM runs");
      check(q.Next() && q.Text(1) == "game-one" && q.Int(2) == 1 && q.Int(3) == 250, "completed run");
      run = q.Text(0);
      Statement tracks(db.get(), "SELECT tracking_id FROM tracks LIMIT 1");
      check(tracks.Next() && tracks.Text(0) == std::to_string(UINT64_MAX), "unsigned track IDs survive");
      Statement gap(db.get(), "SELECT count(*) FROM frames WHERE sample_id=151");
      check(gap.Next() && gap.Int(0) == 0, "policy change gap survives");
      Statement foreign_keys(db.get(), "PRAGMA foreign_key_check");
      check(!foreign_keys.Next(), "foreign keys");
      Statement check_db(db.get(), "PRAGMA integrity_check");
      check(check_db.Next() && check_db.Text(0) == "ok", "database integrity");
    }
    check(
        !std::filesystem::exists(path + "-wal") && !std::filesystem::exists(path + "-journal"),
        "closed publication is one file");
    PlayTrackerTelemetryDb second;
    check(second.Start(directory.string(), {"s", "s"}, {"e", "e"}).ok(), "second start");
    check(second.output_manifest() != path, "new generation never replaces prior data");
    second.Stop();
    writer.MarkRunOutcome(TelemetryRunOutcome::kFailed);
    {
      Database db(path);
      Statement q(db.get(), "SELECT completed FROM runs");
      check(q.Next() && !q.Int(0), "late failure invalidates completed run");
    }
    if (argc == 1)
      std::filesystem::remove_all(directory);
    std::cout << "Database writer tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
