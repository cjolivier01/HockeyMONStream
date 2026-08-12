#include "src/apps/hstream-ui/ScoreboardSelectionDialog.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QResizeEvent>
#include <QtGui/QScreen>
#include <QtGui/QWheelEvent>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kMinimumScale = 0.02;
constexpr double kMaximumScale = 32.0;
constexpr double kPointHitRadius = 15.0;

QVector<QPoint> ordered_points(const QVector<QPoint>& points) {
  if (points.size() != 4)
    return points;
  const auto minimum = [&points](auto value) {
    return *std::min_element(
        points.begin(), points.end(), [&](const QPoint& a, const QPoint& b) { return value(a) < value(b); });
  };
  const auto maximum = [&points](auto value) {
    return *std::max_element(
        points.begin(), points.end(), [&](const QPoint& a, const QPoint& b) { return value(a) < value(b); });
  };
  return {
      minimum([](const QPoint& point) { return point.x() + point.y(); }),
      maximum([](const QPoint& point) { return point.x() - point.y(); }),
      maximum([](const QPoint& point) { return point.x() + point.y(); }),
      minimum([](const QPoint& point) { return point.x() - point.y(); }),
  };
}

} // namespace

ScoreboardSelectionCanvas::ScoreboardSelectionCanvas(QWidget* parent) : QWidget(parent) {
  setObjectName("scoreboardSelectionCanvas");
  setMinimumSize(420, 320);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setCursor(Qt::CrossCursor);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

bool ScoreboardSelectionCanvas::setImage(const QString& path) {
  image_ = QImage(path);
  view_initialized_ = false;
  update();
  return !image_.isNull();
}

void ScoreboardSelectionCanvas::setPoints(const QVector<QPoint>& points) {
  points_.clear();
  if (!image_.isNull()) {
    for (const QPoint& point : points) {
      if (points_.size() == 4)
        break;
      points_.push_back(clampImagePoint(point));
    }
  }
  notifySelectionChanged();
}

const QVector<QPoint>& ScoreboardSelectionCanvas::points() const {
  return points_;
}

double ScoreboardSelectionCanvas::viewScale() const {
  return view_scale_;
}

void ScoreboardSelectionCanvas::fitImage() {
  if (image_.isNull() || width() <= 0 || height() <= 0)
    return;
  const double horizontal = static_cast<double>(width()) / image_.width();
  const double vertical = static_cast<double>(height()) / image_.height();
  view_scale_ = std::clamp(std::min(horizontal, vertical), kMinimumScale, kMaximumScale);
  view_offset_ = {
      (width() - image_.width() * view_scale_) / 2.0,
      (height() - image_.height() * view_scale_) / 2.0,
  };
  view_initialized_ = true;
  update();
}

void ScoreboardSelectionCanvas::actualSize() {
  if (image_.isNull())
    return;
  const QPointF image_center(image_.width() / 2.0, image_.height() / 2.0);
  view_scale_ = 1.0;
  view_offset_ = QPointF(width() / 2.0, height() / 2.0) - image_center;
  view_initialized_ = true;
  update();
}

void ScoreboardSelectionCanvas::focusPoints() {
  if (image_.isNull() || points_.isEmpty())
    return;
  int minimum_x = points_.front().x();
  int maximum_x = minimum_x;
  int minimum_y = points_.front().y();
  int maximum_y = minimum_y;
  for (const QPoint& point : points_) {
    minimum_x = std::min(minimum_x, point.x());
    maximum_x = std::max(maximum_x, point.x());
    minimum_y = std::min(minimum_y, point.y());
    maximum_y = std::max(maximum_y, point.y());
  }
  const double bounds_width = std::max(40, maximum_x - minimum_x);
  const double bounds_height = std::max(40, maximum_y - minimum_y);
  const double scale = std::min(width() * 0.7 / bounds_width, height() * 0.7 / bounds_height);
  view_scale_ = std::clamp(scale, kMinimumScale, kMaximumScale);
  const QPointF center((minimum_x + maximum_x) / 2.0, (minimum_y + maximum_y) / 2.0);
  view_offset_ = QPointF(width() / 2.0, height() / 2.0) - center * view_scale_;
  view_initialized_ = true;
  update();
}

void ScoreboardSelectionCanvas::zoomBy(double factor) {
  setScaleAround(QPointF(width() / 2.0, height() / 2.0), view_scale_ * factor);
}

void ScoreboardSelectionCanvas::undoLastPoint() {
  if (points_.isEmpty())
    return;
  points_.removeLast();
  notifySelectionChanged();
}

void ScoreboardSelectionCanvas::clearPoints() {
  if (points_.isEmpty())
    return;
  points_.clear();
  notifySelectionChanged();
}

void ScoreboardSelectionCanvas::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.fillRect(rect(), QColor(7, 20, 29));
  if (image_.isNull()) {
    painter.setPen(QColor(211, 237, 248));
    painter.drawText(rect(), Qt::AlignCenter, "Could not load the scoreboard image");
    return;
  }
  if (!view_initialized_)
    fitImage();

  painter.save();
  painter.translate(view_offset_);
  painter.scale(view_scale_, view_scale_);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.drawImage(QPointF(0, 0), image_);
  painter.restore();

  const QVector<QPoint> polygon = ordered_points(points_);
  if (polygon.size() >= 2) {
    QPainterPath path;
    path.moveTo(imageToScreen(polygon.front()));
    for (qsizetype index = 1; index < polygon.size(); ++index)
      path.lineTo(imageToScreen(polygon[index]));
    if (polygon.size() == 4)
      path.closeSubpath();
    painter.setPen(QPen(QColor(145, 231, 255), 3));
    painter.drawPath(path);
  }

  for (qsizetype index = 0; index < points_.size(); ++index) {
    const QPointF screen = imageToScreen(points_[index]);
    painter.setPen(QPen(Qt::white, 3));
    painter.setBrush(QColor(255, 91, 111));
    painter.drawEllipse(screen, 9, 9);
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(QRectF(screen.x() - 9, screen.y() - 9, 18, 18), Qt::AlignCenter, QString::number(index + 1));
  }

  if (hover_valid_) {
    const QString coordinates = QString("x=%1, y=%2").arg(hover_point_.x()).arg(hover_point_.y());
    const QFontMetrics metrics(painter.font());
    const QRect text_bounds = metrics.boundingRect(coordinates).adjusted(-9, -6, 9, 6);
    QRect box(QPoint(14, 14), text_bounds.size());
    painter.setPen(QColor(159, 231, 255, 80));
    painter.setBrush(QColor(5, 16, 24, 220));
    painter.drawRoundedRect(box, 8, 8);
    painter.setPen(QColor(223, 247, 255));
    painter.drawText(box, Qt::AlignCenter, coordinates);
  }
}

void ScoreboardSelectionCanvas::resizeEvent(QResizeEvent* event) {
  if (image_.isNull()) {
    QWidget::resizeEvent(event);
    return;
  }
  if (!view_initialized_ || event->oldSize().width() <= 0 || event->oldSize().height() <= 0) {
    fitImage();
  } else {
    const QPointF old_center(event->oldSize().width() / 2.0, event->oldSize().height() / 2.0);
    const QPointF image_center = screenToImage(old_center);
    view_offset_ = QPointF(event->size().width() / 2.0, event->size().height() / 2.0) - image_center * view_scale_;
    update();
  }
  QWidget::resizeEvent(event);
}

void ScoreboardSelectionCanvas::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || image_.isNull()) {
    QWidget::mousePressEvent(event);
    return;
  }
  pointer_active_ = true;
  pointer_moved_ = false;
  press_position_ = event->position();
  pan_start_offset_ = view_offset_;
  dragged_point_ = pointNear(event->position());
  setCursor(dragged_point_ >= 0 ? Qt::SizeAllCursor : Qt::ClosedHandCursor);
  event->accept();
}

void ScoreboardSelectionCanvas::mouseMoveEvent(QMouseEvent* event) {
  if (image_.isNull())
    return;
  hover_point_ = clampImagePoint(screenToImage(event->position()));
  hover_valid_ = true;
  if (hoverChanged)
    hoverChanged(hover_point_, true);
  if (!pointer_active_) {
    setCursor(pointNear(event->position()) >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
    update();
    return;
  }
  if ((event->position() - press_position_).manhattanLength() > 3)
    pointer_moved_ = true;
  if (dragged_point_ >= 0) {
    points_[dragged_point_] = clampImagePoint(screenToImage(event->position()));
    notifySelectionChanged();
  } else {
    view_offset_ = pan_start_offset_ + event->position() - press_position_;
    update();
  }
  event->accept();
}

void ScoreboardSelectionCanvas::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !pointer_active_) {
    QWidget::mouseReleaseEvent(event);
    return;
  }
  if (dragged_point_ < 0 && !pointer_moved_ && points_.size() < 4) {
    points_.push_back(clampImagePoint(screenToImage(event->position())));
    notifySelectionChanged();
  }
  pointer_active_ = false;
  dragged_point_ = -1;
  setCursor(pointNear(event->position()) >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
  event->accept();
}

void ScoreboardSelectionCanvas::leaveEvent(QEvent* event) {
  hover_valid_ = false;
  if (hoverChanged)
    hoverChanged({}, false);
  update();
  QWidget::leaveEvent(event);
}

void ScoreboardSelectionCanvas::wheelEvent(QWheelEvent* event) {
  if (image_.isNull() || event->angleDelta().y() == 0) {
    QWidget::wheelEvent(event);
    return;
  }
  const double factor = std::pow(1.0015, event->angleDelta().y());
  setScaleAround(event->position(), view_scale_ * factor);
  event->accept();
}

QPoint ScoreboardSelectionCanvas::clampImagePoint(const QPointF& point) const {
  if (image_.isNull())
    return {};
  return {
      std::clamp(qRound(point.x()), 0, image_.width() - 1),
      std::clamp(qRound(point.y()), 0, image_.height() - 1),
  };
}

QPointF ScoreboardSelectionCanvas::imageToScreen(const QPoint& point) const {
  return QPointF(point) * view_scale_ + view_offset_;
}

QPointF ScoreboardSelectionCanvas::screenToImage(const QPointF& point) const {
  return (point - view_offset_) / view_scale_;
}

int ScoreboardSelectionCanvas::pointNear(const QPointF& position) const {
  for (qsizetype index = points_.size(); index > 0; --index) {
    const qsizetype candidate = index - 1;
    const QPointF delta = imageToScreen(points_[candidate]) - position;
    if (std::hypot(delta.x(), delta.y()) <= kPointHitRadius)
      return static_cast<int>(candidate);
  }
  return -1;
}

void ScoreboardSelectionCanvas::setScaleAround(const QPointF& position, double scale) {
  if (image_.isNull())
    return;
  const QPointF image_position = screenToImage(position);
  view_scale_ = std::clamp(scale, kMinimumScale, kMaximumScale);
  view_offset_ = position - image_position * view_scale_;
  view_initialized_ = true;
  update();
}

void ScoreboardSelectionCanvas::notifySelectionChanged() {
  update();
  if (selectionChanged)
    selectionChanged();
}

ScoreboardSelectionDialog::ScoreboardSelectionDialog(
    const QUrl& selector_url,
    const QString& image_path,
    const QVector<QPoint>& initial_points,
    QWidget* parent)
    : QDialog(parent), selector_url_(selector_url), image_path_(image_path), initial_points_(initial_points) {
  setObjectName("scoreboardSelectionDialog");
  setWindowTitle("Select Scoreboard Corners");
  setWindowModality(Qt::ApplicationModal);
  setAttribute(Qt::WA_DeleteOnClose);
  buildUi();
  if (QScreen* screen = QApplication::primaryScreen()) {
    const QSize available = screen->availableGeometry().size();
    resize(
        std::min(1280, static_cast<int>(available.width() * 0.92)),
        std::min(820, static_cast<int>(available.height() * 0.90)));
  } else {
    resize(1100, 720);
  }
}

QString ScoreboardSelectionDialog::loadError() const {
  return load_error_;
}

void ScoreboardSelectionDialog::closeAfterBackendCompletion() {
  backend_completed_ = true;
  if (pending_reply_)
    pending_reply_->abort();
  QDialog::accept();
}

void ScoreboardSelectionDialog::reject() {
  requestCancel();
}

void ScoreboardSelectionDialog::closeEvent(QCloseEvent* event) {
  if (backend_completed_) {
    event->accept();
    QDialog::closeEvent(event);
    return;
  }
  event->ignore();
  requestCancel();
}

void ScoreboardSelectionDialog::buildUi() {
  setStyleSheet(
      "QDialog { background: #081c27; color: #eefaff; }"
      "QLabel { color: #d7edf6; }"
      "QPushButton { padding: 7px 11px; }"
      "QPushButton#scoreboardSaveButton { background: #167a57; color: white; font-weight: bold; }"
      "QPushButton#scoreboardNoScoreboardButton { background: #8f3040; color: white; }"
      "QFrame#scoreboardStatusPanel { background: #102d3b; border: 1px solid #285063; border-radius: 6px; }");

  auto* root = new QVBoxLayout(this);
  auto* title = new QLabel("Select the four scoreboard corners", this);
  QFont title_font = title->font();
  title_font.setPointSize(title_font.pointSize() + 5);
  title_font.setBold(true);
  title->setFont(title_font);
  root->addWidget(title);
  auto* instructions = new QLabel(
      "Click corners in any order. Drag red points to refine them, drag empty ice to pan, and use the mouse wheel "
      "or zoom controls for precision.",
      this);
  instructions->setWordWrap(true);
  root->addWidget(instructions);

  auto* splitter = new QSplitter(Qt::Horizontal, this);
  canvas_ = new ScoreboardSelectionCanvas(splitter);
  splitter->addWidget(canvas_);

  auto* panel = new QWidget(splitter);
  panel->setMinimumWidth(285);
  panel->setMaximumWidth(390);
  auto* panel_layout = new QVBoxLayout(panel);
  selection_count_ = new QLabel(panel);
  selection_count_->setObjectName("scoreboardSelectionCount");
  QFont count_font = selection_count_->font();
  count_font.setBold(true);
  selection_count_->setFont(count_font);
  panel_layout->addWidget(selection_count_);

  auto* status_panel = new QFrame(panel);
  status_panel->setObjectName("scoreboardStatusPanel");
  auto* status_layout = new QVBoxLayout(status_panel);
  status_title_ = new QLabel(status_panel);
  status_title_->setObjectName("scoreboardStatusTitle");
  QFont status_font = status_title_->font();
  status_font.setBold(true);
  status_title_->setFont(status_font);
  status_message_ = new QLabel(status_panel);
  status_message_->setObjectName("scoreboardStatusMessage");
  status_message_->setWordWrap(true);
  status_layout->addWidget(status_title_);
  status_layout->addWidget(status_message_);
  panel_layout->addWidget(status_panel);

  auto* zoom_row = new QGridLayout;
  auto add_button = [&](const QString& text, const char* name, int row, int column, auto callback) {
    auto* button = new QPushButton(text, panel);
    button->setObjectName(name);
    connect(button, &QPushButton::clicked, this, callback);
    zoom_row->addWidget(button, row, column);
    return button;
  };
  add_button("Zoom Out", "scoreboardZoomOutButton", 0, 0, [this]() { canvas_->zoomBy(0.8); });
  add_button("Zoom In", "scoreboardZoomInButton", 0, 1, [this]() { canvas_->zoomBy(1.25); });
  add_button("Fit Image", "scoreboardFitButton", 1, 0, [this]() { canvas_->fitImage(); });
  add_button("100% Zoom", "scoreboardActualSizeButton", 1, 1, [this]() { canvas_->actualSize(); });
  focus_button_ = add_button("Focus Points", "scoreboardFocusButton", 2, 0, [this]() { canvas_->focusPoints(); });
  undo_button_ = add_button("Undo Last Point", "scoreboardUndoButton", 2, 1, [this]() {
    canvas_->undoLastPoint();
    status_title_->setText("Last point removed");
    status_message_->setText("Click to place it again or drag another point into place.");
  });
  clear_button_ = add_button("Clear Points", "scoreboardClearButton", 3, 0, [this]() {
    canvas_->clearPoints();
    status_title_->setText("Points cleared");
    status_message_->setText("Click four scoreboard corners to start again.");
  });
  no_scoreboard_button_ = add_button("No Scoreboard", "scoreboardNoScoreboardButton", 3, 1, [this]() {
    if (QMessageBox::question(
            this,
            "Confirm no scoreboard",
            "Mark this game as having no scoreboard? The scoreboard overlay will be disabled.",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes) {
      submit(Submission::kNoScoreboard);
    }
  });
  panel_layout->addLayout(zoom_row);

  auto* points_title = new QLabel("Selected image coordinates", panel);
  QFont points_font = points_title->font();
  points_font.setBold(true);
  points_title->setFont(points_font);
  panel_layout->addWidget(points_title);
  for (int index = 0; index < 4; ++index) {
    auto* value = new QLabel(panel);
    value->setObjectName(QString("scoreboardPoint%1Value").arg(index + 1));
    point_values_.push_back(value);
    panel_layout->addWidget(value);
  }
  hover_coordinates_ = new QLabel("Pointer: outside image", panel);
  hover_coordinates_->setObjectName("scoreboardHoverCoordinates");
  panel_layout->addWidget(hover_coordinates_);
  panel_layout->addStretch();
  splitter->addWidget(panel);
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 0);
  root->addWidget(splitter, 1);

  auto* action_row = new QHBoxLayout;
  action_row->addStretch();
  cancel_button_ = new QPushButton("Cancel", this);
  cancel_button_->setObjectName("scoreboardCancelButton");
  connect(cancel_button_, &QPushButton::clicked, this, [this]() { requestCancel(); });
  save_button_ = new QPushButton("Save Selection", this);
  save_button_->setObjectName("scoreboardSaveButton");
  connect(save_button_, &QPushButton::clicked, this, [this]() { submit(Submission::kSave); });
  action_row->addWidget(cancel_button_);
  action_row->addWidget(save_button_);
  root->addLayout(action_row);

  network_ = new QNetworkAccessManager(this);
  canvas_->selectionChanged = [this]() {
    refreshSelectionUi();
    if (submitting_ || !load_error_.isEmpty())
      return;
    const qsizetype count = canvas_->points().size();
    if (count == 4) {
      status_title_->setText("Ready to save");
      status_message_->setText("All four corners are set. Save the selection when it looks right.");
    } else if (count > 0) {
      status_title_->setText("Point added");
      status_message_->setText(QString("%1 corner%2 left to place. Drag any red point to refine it.")
                                   .arg(4 - count)
                                   .arg(count == 3 ? "" : "s"));
    }
  };
  canvas_->hoverChanged = [this](const QPoint& point, bool valid) { updateHover(point, valid); };
  if (!canvas_->setImage(image_path_)) {
    load_error_ = QString("Could not load scoreboard selector image: %1").arg(image_path_);
    status_title_->setText("Image failed to load");
    status_message_->setText("Stop the pipeline, verify s.png exists and is readable, then try again.");
  } else {
    canvas_->setPoints(initial_points_);
    status_title_->setText(initial_points_.isEmpty() ? "Place four points" : "Existing selection loaded");
    status_message_->setText(
        initial_points_.isEmpty() ? "Click each scoreboard corner. You can drag a point after placing it."
                                  : "Review or adjust the existing points, then save the selection.");
    QTimer::singleShot(0, canvas_, [this]() {
      if (canvas_->points().isEmpty())
        canvas_->fitImage();
      else
        canvas_->focusPoints();
    });
  }
  refreshSelectionUi();
}

void ScoreboardSelectionDialog::refreshSelectionUi() {
  const QVector<QPoint>& points = canvas_->points();
  canvas_->setEnabled(load_error_.isEmpty() && !submitting_);
  selection_count_->setText(QString("%1 / 4 points").arg(points.size()));
  for (qsizetype index = 0; index < point_values_.size(); ++index) {
    point_values_[index]->setText(
        index < points.size()
            ? QString("Point %1: (%2, %3)").arg(index + 1).arg(points[index].x()).arg(points[index].y())
            : QString("Point %1: Not set").arg(index + 1));
  }
  const bool have_image = load_error_.isEmpty();
  save_button_->setEnabled(have_image && !submitting_ && points.size() == 4);
  no_scoreboard_button_->setEnabled(!submitting_);
  focus_button_->setEnabled(have_image && !points.isEmpty());
  undo_button_->setEnabled(have_image && !submitting_ && !points.isEmpty());
  clear_button_->setEnabled(have_image && !submitting_ && !points.isEmpty());
  cancel_button_->setEnabled(!submitting_);
}

void ScoreboardSelectionDialog::updateHover(const QPoint& point, bool valid) {
  hover_coordinates_->setText(
      valid ? QString("Pointer: x=%1, y=%2").arg(point.x()).arg(point.y()) : "Pointer: outside image");
}

void ScoreboardSelectionDialog::submit(Submission submission) {
  if (submitting_ || (submission == Submission::kSave && canvas_->points().size() != 4))
    return;
  QUrl endpoint = endpointFor(submission);
  if (!endpoint.isValid() || endpoint.scheme() != "http" || endpoint.host().isEmpty() || endpoint.port() <= 0) {
    status_title_->setText("Could not submit selection");
    status_message_->setText("The pipeline supplied an invalid private selector address.");
    return;
  }
  QJsonObject body;
  if (submission == Submission::kSave) {
    QJsonArray points;
    for (const QPoint& point : canvas_->points())
      points.append(QJsonArray{point.x(), point.y()});
    body.insert("points", points);
  }
  QNetworkRequest request(endpoint);
  request.setTransferTimeout(10000);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setRawHeader(
      "Origin",
      QString("%1://%2:%3").arg(selector_url_.scheme(), selector_url_.host()).arg(selector_url_.port()).toUtf8());
  submitting_ = true;
  status_title_->setText(submission == Submission::kCancel ? "Cancelling selection" : "Saving selection");
  status_message_->setText("Waiting for the stitching pipeline to accept the selection.");
  refreshSelectionUi();
  pending_reply_ = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
  QNetworkReply* reply = pending_reply_;
  connect(reply, &QNetworkReply::finished, this, [this, reply, submission]() { finishSubmission(reply, submission); });
}

void ScoreboardSelectionDialog::finishSubmission(QNetworkReply* reply, Submission submission) {
  if (backend_completed_) {
    reply->deleteLater();
    return;
  }
  pending_reply_.clear();
  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QString response = QString::fromUtf8(reply->readAll()).trimmed();
  const QString network_error = reply->errorString();
  const bool succeeded = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
  reply->deleteLater();
  if (succeeded) {
    backend_completed_ = true;
    if (submission == Submission::kCancel)
      QDialog::reject();
    else
      QDialog::accept();
    return;
  }
  submitting_ = false;
  status_title_->setText(submission == Submission::kCancel ? "Could not cancel" : "Could not save");
  status_message_->setText(
      !response.isEmpty() ? response : QString("The pipeline did not accept the request: %1").arg(network_error));
  refreshSelectionUi();
}

void ScoreboardSelectionDialog::requestCancel() {
  if (backend_completed_) {
    QDialog::reject();
    return;
  }
  submit(Submission::kCancel);
}

QUrl ScoreboardSelectionDialog::endpointFor(Submission submission) const {
  QUrl endpoint(selector_url_);
  switch (submission) {
    case Submission::kSave:
      endpoint.setPath("/save");
      break;
    case Submission::kNoScoreboard:
      endpoint.setPath("/none");
      break;
    case Submission::kCancel:
      endpoint.setPath("/cancel");
      break;
  }
  return endpoint;
}
