#include "src/apps/hstream-ui/TelemetryDbPublisher.h"
#include <unistd.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
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
    check(writer.Start(work.toStdString(), {"s", "s"}, {"e", "e"}, {}, 2048, "game-one").ok(), "writer");
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
        QDir(game).entryList(QDir::Files) == QStringList{"game-one_telemetry-2.db"},
        "one database copied with recorded game ID and exact suffix");
    check(!telemetry_csv_destination_paths_available(game, "-2"), "video suffix selection sees database collisions");
    check(!publish_telemetry_database(source, game, QString("-2")).ok, "cannot overwrite previous recording");
    check(!publish_telemetry_database(source, game, QString("/../bad")).ok, "reject invalid suffix");
    for (const QString& suffix : {QString(""), QString("-0"), QString("-000")})
      check(!publish_telemetry_database(source, game, suffix).ok, "reject unnumbered and zero versions");
    const QString fresh_game = root.filePath("fresh-game");
    check(QDir().mkpath(fresh_game), "fresh game directory");
    const auto initial = publish_telemetry_database(source, fresh_game);
    check(initial.ok && initial.published_paths.front().endsWith("game-one_telemetry-1.db"), "first version is one");
    const auto padded = publish_telemetry_database(source, fresh_game, QString("-007"));
    check(
        padded.ok && padded.published_paths.front().endsWith("game-one_telemetry-007.db"),
        "preserve positive suffix spelling");
    const QString symlink_target_game = root.filePath("symlink-target-game");
    const QString symlink_game = root.filePath("symlink-game");
    check(QDir().mkpath(symlink_target_game), "symlink target game directory");
    check(
        ::symlink(QFile::encodeName(symlink_target_game).constData(), QFile::encodeName(symlink_game).constData()) == 0,
        "symlink game directory");
    check(telemetry_csv_destination_paths_available(symlink_game, "-5"), "symlinked game directory suffix preflight");
    const auto through_symlink = publish_telemetry_database(source, symlink_game, QString("-5"));
    check(through_symlink.ok, through_symlink.error.toStdString());
    check(
        QFileInfo::exists(QDir(symlink_target_game).filePath("game-one_telemetry-5.db")),
        "database published through symlinked game directory");
    check(
        !telemetry_csv_destination_paths_available(symlink_game, "-5"),
        "symlinked game directory suffix collision is visible after database publication");
    const auto second = publish_telemetry_database(source, game);
    check(
        second.ok && second.published_paths.front().endsWith("game-one_telemetry-3.db"),
        "standalone publication chooses an available suffix");
    const auto third = publish_telemetry_database(source, game);
    check(third.ok && third.published_paths.front().endsWith("game-one_telemetry-4.db"), "standalone next generation");
    QFile legacy(root.filePath("game/hstream_telemetry-12.db"));
    check(legacy.open(QIODevice::WriteOnly), "reserve legacy generation");
    legacy.close();
    check(!telemetry_csv_destination_paths_available(game, "-12"), "legacy database suffix remains occupied");
    check(!publish_telemetry_database(source, game, QString("-12")).ok, "reject fixed legacy database generation");
    check(
        !publish_telemetry_database(source, game, QString("-0012")).ok,
        "leading zeroes cannot bypass legacy generation collision");
    check(!QFileInfo::exists(QDir(game).filePath("game-one_telemetry-12.db")), "collision leaves no new marker");
    const auto after_legacy = publish_telemetry_database(source, game);
    check(
        after_legacy.ok && after_legacy.published_paths.front().endsWith("game-one_telemetry-13.db"),
        "numbering advances beyond legacy database generations");
    QFile sqlite_alias(root.filePath("game/hm_telemetry-014.sqlite"));
    check(sqlite_alias.open(QIODevice::WriteOnly), "reserve legacy SQLite generation");
    sqlite_alias.close();
    check(!publish_telemetry_database(source, game, QString("-14")).ok, "reject legacy SQLite generation alias");
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
