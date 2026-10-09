#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <iostream>
#include "src/apps/hstream-ui/HighlightItemEditor.h"
#include "src/apps/hstream-ui/HighlightReelPipeline.h"
#include "src/apps/hstream-ui/HighlightTracking.h"

namespace {
qint64 inspected = -1, queried = -1;
QString attached;
bool active = false;
} // namespace

// The real dialog drives deterministic inspection and crossing-player fixtures.
// This test needs X11 window IDs but no decoder, GPU or telemetry database.
namespace hm::ui {
struct HighlightReelPipeline::Impl {};
HighlightReelPipeline::HighlightReelPipeline() = default;
HighlightReelPipeline::~HighlightReelPipeline() = default;
bool HighlightReelPipeline::PreviewAvailable() {
  return true;
}
bool HighlightReelPipeline::Start(Request request, QString*) {
  inspected = request.inspect_game_ms;
  active = true;
  return true;
}
void HighlightReelPipeline::Cancel() {
  active = false;
}
HighlightReelPipeline::Status HighlightReelPipeline::Poll() const {
  Status status;
  status.finished = true;
  status.cancelled = !active;
  return status;
}
QStringList HighlightTrackingRuns(const QString&, const QString&, QString*) {
  return {"run"};
}
QString HighlightTrackingDatabasePath(const QString& directory, const QString& game, const QString&) {
  return directory + "/" + game + "_telemetry-1.db";
}
bool HighlightTrackingChoices(
    const QString&,
    const QString&,
    const QString&,
    qint64 time,
    qint64,
    QVector<HighlightTrackChoice>* choices,
    QString*) {
  queried = time;
  HighlightTrackChoice left, right;
  left.id = "42";
  left.box = QRectF(.15, .35, .1, .2);
  right.id = "99";
  right.box = QRectF(.75, .35, .1, .2);
  *choices = {left, right};
  return true;
}
bool CaptureHighlightTrack(HighlightAnnotation* cue, const HighlightTrackChoice& choice, const QString&, QString*) {
  attached = choice.id;
  cue->track_id = choice.id;
  cue->positions = {{cue->start_ms, .2, .4}};
  cue->motion = "track";
  return true;
}
} // namespace hm::ui

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir directory;
  const QString archive = directory.filePath("archive.mp4");
  QFile file(archive);
  if (!file.open(QIODevice::WriteOnly) || file.write("fixture") < 0)
    return 1;
  file.close();
  hm::ui::HighlightInterval clip;
  clip.event_mode = false;
  clip.start_ms = 10000;
  clip.end_ms = 20000;
  hm::ui::HighlightAnnotation cue;
  cue.start_ms = 10000;
  cue.end_ms = 12000;
  clip.annotations = {cue};
  bool ok = true;
  QTimer::singleShot(0, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    auto action = [&](const QString& name) {
      for (auto* button : dialog->findChildren<QPushButton*>())
        if (button->text() == name) {
          button->click();
          return;
        }
      ok = false;
    };
    QDoubleSpinBox* inspector = nullptr;
    QWidget* target = nullptr;
    for (auto* box : dialog->findChildren<QDoubleSpinBox*>())
      if (box->minimum() == 0 && box->maximum() == 9.999)
        inspector = box;
    for (auto* widget : dialog->findChildren<QWidget*>())
      if (widget->minimumSize() == QSize(320, 180))
        target = widget;
    if (!inspector || !target) {
      ok = false;
      dialog->reject();
      return;
    }
    auto inspect = [&](double time) {
      inspector->setValue(time);
      action("Inspect / refresh frame");
      for (int i = 0; i < 5; ++i) {
        app.processEvents();
        QThread::msleep(40);
      }
      app.processEvents();
    };
    auto click = [&](double x, double y) {
      double w = target->width(), h = target->height();
      if (w / h > 16.0 / 9)
        w = h * 16.0 / 9;
      else
        h = w * 9.0 / 16;
      QPointF position((target->width() - w) / 2 + x * w, (target->height() - h) / 2 + y * h);
      QMouseEvent event(QEvent::MouseButtonPress, position, position, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(target, &event);
    };
    inspect(8);
    action("Load runs");
    action("Find tracks");
    for (auto* box : dialog->findChildren<QCheckBox*>())
      if (box->text() == "This recorded run produced the selected archive")
        box->setChecked(true);
    // ID 42 has crossed to the right in the later frame. The old boxes must
    // never let that click select ID 99 or leave a default ID ready to attach.
    click(.8, .4);
    action("Attach selected track");
    ok &= inspected == 18000 && queried == 10000 && attached.isEmpty();
    inspect(0);
    // Changing the requested time alone leaves the old image visible.
    inspector->setValue(1);
    click(.8, .4);
    action("Attach selected track");
    ok &= attached.isEmpty();
    inspector->setValue(0);
    click(.2, .4);
    click(.5, .4);
    action("Attach selected track");
    ok &= attached.isEmpty();
    click(.2, .4);
    action("Attach selected track");
    ok &= attached == "42";
    action("Stop");
    click(.8, .4);
    ok &= dialog->findChild<QLabel*>("highlightItemStatus")->text().contains("Inspect / refresh");
    dialog->reject();
  });
  hm::ui::EditHighlightItem(&clip, directory.path(), "game", archive, "stitched", 0, {}, nullptr);
  if (!ok)
    std::cerr << "Track clicks used stale or unavailable inspection frames\n";
  return ok ? 0 : 1;
}
