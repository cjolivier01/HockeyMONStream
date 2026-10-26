#include "src/apps/hstream-ui/StitchingStillPreview.h"

#include "src/apps/hstream-ui/StitchingStillImage.h"

#include <QtCore/QEvent>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QPainter>

#include <exception>
#include <memory>

StitchingStillPreview::StitchingStillPreview(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setGeometry(parent->rect());
  parent->installEventFilter(this);
  hide();
  auto* timer = new QTimer(this);
  timer->setInterval(1000);
  connect(timer, &QTimer::timeout, this, [this] { refresh(); });
  timer->start();
}

StitchingStillPreview::~StitchingStillPreview() {
  cancelRead();
}

void StitchingStillPreview::cancelRead() {
  if (!read_state_)
    return;
  read_state_->cancelled.store(true);
  // Do not join the worker: it may already be decoding an opened image. This
  // barrier drains only our metadata read and forbids a queued stale read from
  // acquiring publication locks after the caller starts its own transaction.
  std::lock_guard<std::mutex> metadata_guard(read_state_->metadata_mutex);
}

void StitchingStillPreview::setSource(
    const QString& directory,
    const QString& expected_generation,
    const QByteArray& expected_settings) {
  if (directory_ == directory && expected_generation_ == expected_generation && expected_settings_ == expected_settings)
    return;
  directory_ = directory;
  expected_generation_ = expected_generation;
  expected_settings_ = expected_settings;
  ++request_;
  revision_.clear();
  image_ = {};
  setProperty("calibrationImage", QVariant());
  hide();
  cancelRead();
  refresh();
}

void StitchingStillPreview::refresh() {
  if (directory_.isEmpty())
    return;
  const auto current = StitchingStillRevision(directory_);
  if (revision_ == current)
    return;
  image_ = {};
  setProperty("calibrationImage", QVariant());
  hide();
  if (worker_)
    return;
  const auto request = request_;
  const auto directory = directory_;
  const auto expected = expected_generation_;
  const auto settings = expected_settings_;
  const auto state = std::make_shared<ReadState>();
  read_state_ = state;
  const auto metadata_locked = metadata_locked_observer_;
  const auto decoding = decoding_observer_;
  auto result = std::make_shared<SavedStitchingStill>();
  worker_ = QThread::create([directory, expected, settings, current, result, state, metadata_locked, decoding] {
    try {
      *result = ReadSavedStitchingStill(
          directory,
          expected,
          settings,
          current,
          state->metadata_mutex,
          [state] { return state->cancelled.load(); },
          metadata_locked,
          decoding);
    } catch (const std::exception&) {
      // A failed optional image must not terminate playback or the UI.
      *result = {};
    }
  });
  connect(worker_, &QThread::finished, this, [this, result, request, current] {
    worker_ = nullptr;
    if (request != request_ || directory_.isEmpty() || StitchingStillRevision(directory_) != current)
      return;
    if (!result->retry)
      revision_ = current;
    image_ = std::move(result->image);
    if (!image_.isNull()) {
      setProperty("calibrationImage", result->name);
      setAccessibleName("Saved stitching calibration image");
      setToolTip("Saved calibration image; Play starts the live stitched preview.");
      show();
      raise();
      update();
    }
  });
  connect(worker_, &QThread::finished, worker_, &QObject::deleteLater);
  worker_->start();
}

bool StitchingStillPreview::eventFilter(QObject* watched, QEvent* event) {
  if (watched == parentWidget() && event->type() == QEvent::Resize)
    setGeometry(parentWidget()->rect());
  return QWidget::eventFilter(watched, event);
}

void StitchingStillPreview::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.fillRect(rect(), Qt::black);
  if (image_.isNull())
    return;
  const QSize size = image_.size().scaled(this->size(), Qt::KeepAspectRatio);
  const QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.drawImage(target, image_);
}
