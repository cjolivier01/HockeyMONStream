#include "src/apps/hstream-ui/HighlightTracking.h"
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <cmath>
#include <iostream>
#include "hstream/src/libs/recording/Database.h"
#include "src/apps/hstream-ui/HighlightScene.h"
using namespace hm::ui;
int main() {
  QTemporaryDir dir;
  const QString path = dir.filePath("tracks.sqlite");
  QString error;
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return 1;
  file.close();
  hm::recording::Database db(path.toStdString(), true);
  db.Exec(hm::recording::Schema());
  db.Exec("INSERT INTO runs VALUES('run','game','2026-10-08','native','{}','{}',1,'success',3)");
  db.Exec("INSERT INTO geometries VALUES('run',1,1000,500,'stitched','revision',NULL,NULL,NULL,'{}')");
  for (int i = 1; i <= 3; ++i) {
    const int time = i == 2 ? 1033 : 1000, seek = i == 3 ? 1 : 0;
    db.Exec(
        "INSERT INTO frames VALUES('run'," + std::to_string(i) + ",0," + std::to_string(i) + ",NULL,NULL," +
        std::to_string(time * 1000000) + ",NULL," + std::to_string(seek) + ",0,1,1,1)");
    db.Exec(
        "INSERT INTO tracks VALUES('run'," + std::to_string(i) + ",0,'42'," + std::to_string(i == 3 ? 700 : 200) +
        ",100,100,200,1,0,'{}')");
    db.Exec("INSERT INTO cameras VALUES('run'," + std::to_string(i) + ",'program',100,50,400,300)");
    db.Exec("INSERT INTO replay_frames VALUES('run'," + std::to_string(i) + ",0,0,1000,500,1,1,10,10)");
  }
  if (HighlightTrackingRuns(path, "game", &error) != QStringList{"run"} ||
      !HighlightTrackingRuns(path, "other", &error).isEmpty())
    return 1;
  QVector<HighlightTrackChoice> choices;
  if (!HighlightTrackingChoices(path, "run", "stitched", 1000, 0, &choices, &error) || choices.size() != 2) {
    std::cerr << error.toStdString();
    return 1;
  }
  HighlightAnnotation a;
  a.database = path;
  a.run_id = "run";
  a.archive_path = "archive.mp4";
  a.start_ms = 1000;
  a.end_ms = 2000;
  for (const auto& choice : choices)
    if (choice.seek == 0) {
      if (!CaptureHighlightTrack(&a, choice, "stitched", &error) || a.positions.size() != 2 ||
          a.positions[0].x != .25 || a.positions[1].time_ms != 1033) {
        std::cerr << "Capture crossed seek context\n";
        return 1;
      }
    }
  a.blink = false;
  if (HighlightAnchor(a, 1300)) {
    std::cerr << "Cue bridged recorded gap\n";
    return 1;
  }
  if (!HighlightTrackingChoices(path, "run", "program", 1000, 0, &choices, &error))
    return 1;
  for (const auto& choice : choices)
    if (choice.seek == 0) {
      if (!CaptureHighlightTrack(&a, choice, "program", &error))
        return 1;
      // Rotation is 4 degrees around camera center (300,200), then crop/scale.
      const double radians = 4 * M_PI / 180;
      const double expected = (300 - 50 * std::cos(radians) + 100 * std::sin(radians) - 100) / 400;
      if (std::abs(a.positions[0].x - expected) > 1e-8) {
        std::cerr << "Program rotation/crop mismatch\n";
        return 1;
      }
      const QPointF shoulder(
          (300 - 5 * std::cos(radians) + 95 * std::sin(radians) - 100) / 400,
          (200 - 5 * std::sin(radians) - 95 * std::cos(radians) - 50) / 300);
      if (!choice.box.contains(shoulder)) {
        std::cerr << "Rotated player shoulder excluded from click selection\n";
        return 1;
      }
    }
  if (HighlightTrackingChoices(path, "run", "stitched", 5000, 0, &choices, &error)) {
    std::cerr << "Missing time accepted\n";
    return 1;
  }
  return 0;
}
