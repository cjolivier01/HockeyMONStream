#include "src/apps/hstream-ui/TelemetryDbPublisher.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include "hstream/src/gst-plugins/gst-videoprep/playtracker/PlayTrackerTelemetryDb.h"
#include "hstream/src/libs/recording/Database.h"
using namespace hm::playtracker;
using namespace hm::ui_internal;
void check(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir root;
    check(root.isValid(), "temporary directory");
    const QString work = root.filePath("work"), game = root.filePath("game");
    QDir().mkpath(game);
    PlayTrackerTelemetryDb writer;
    check(writer.Start(work.toStdString(), {"s", "s"}, {"e", "e"}).ok(), "writer");
    const QString source = QString::fromStdString(writer.output_manifest());
    check(!publish_telemetry_database(source, game).ok, "reject incomplete source");
    TelemetrySample s;
    s.width = 100;
    s.height = 50;
    s.pts_ns = 0;
    s.policy_boxes.push_back({0, 0, 90, 40});
    check(writer.TryEnqueue(s), "sample");
    writer.MarkRunOutcome(TelemetryRunOutcome::kEndOfStream);
    writer.Stop();
    const auto first = publish_telemetry_database(source, game, QString("-2"));
    check(first.ok, first.error.toStdString());
    check(
        QDir(game).entryList(QDir::Files) == QStringList{"hstream_telemetry-2.db"},
        "one database copied with exact suffix");
    check(!telemetry_csv_destination_paths_available(game, "-2"), "video suffix selection sees database collisions");
    check(!publish_telemetry_database(source, game, QString("-2")).ok, "cannot overwrite previous recording");
    check(!publish_telemetry_database(source, game, QString("/../bad")).ok, "reject invalid suffix");
    const auto second = publish_telemetry_database(source, game);
    check(
        second.ok && second.published_paths.front().endsWith("hstream_telemetry-3.db"),
        "standalone publication chooses an available suffix");
    const auto third = publish_telemetry_database(source, game);
    check(third.ok && third.published_paths.front().endsWith("hstream_telemetry-4.db"), "standalone next generation");
    hm::recording::Database copy(first.published_paths.front().toStdString());
    hm::recording::Database original(source.toStdString());
    hm::recording::Statement a(copy.get(), "SELECT run_id FROM runs"), b(original.get(), "SELECT run_id FROM runs");
    check(a.Next() && b.Next() && a.Text(0) == b.Text(0), "publication preserves recording GUID");
    std::cout << "Database publication tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
