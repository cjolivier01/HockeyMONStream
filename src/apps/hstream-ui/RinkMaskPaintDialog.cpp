#include "src/apps/hstream-ui/RinkMaskPaintDialog.h"
#include "src/apps/hstream-ui/PreviewDialogWindow.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <functional>

class RinkMaskCanvas : public QWidget {
 public:
  RinkMaskCanvas(const QImage& image, const QImage& source) : image_(image), source_(source) {
    // Stroke coordinates are drawn at pixel centers. Keep the texture aligned to the underlying still.
    source_.setTransform(QTransform::fromTranslate(-.5, -.5));
    setMouseTracking(true);
    setMinimumSize(200, 120);
    setObjectName("rinkMaskCanvas");
  }
  void setDiameter(int pixels) {
    diameter_ = pixels;
    update();
  }
  void setErase(bool erase) {
    erase_ = erase;
  }
  void undo() {
    if (!strokes_.empty()) {
      points_ -= strokes_.back().points.size();
      strokes_.pop_back();
    }
    drawing_ = false;
    update();
  }
  QByteArray response() const {
    if (strokes_.empty())
      return "keep\n";
    QByteArray out;
    for (const auto& stroke : strokes_) {
      out += QByteArray::number(stroke.erase ? -stroke.diameter : stroke.diameter);
      for (const auto& point : stroke.points)
        out += ' ' + QByteArray::number(point.x()) + ' ' + QByteArray::number(point.y());
      out += '\n';
    }
    return out;
  }
  std::function<void(bool, bool)> changed;

 protected:
  QRectF target() const {
    const QSize size = image_.size().scaled(this->size(), Qt::KeepAspectRatio);
    return QRectF((width() - size.width()) / 2., (height() - size.height()) / 2., size.width(), size.height());
  }
  void paintEvent(QPaintEvent*) override {
    QPainter paint(this);
    paint.fillRect(rect(), Qt::black);
    const auto area = target();
    paint.drawImage(area, image_);
    paint.translate(area.topLeft());
    paint.scale(area.width() / image_.width(), area.height() / image_.height());
    paint.setClipRect(image_.rect());
    paint.setRenderHint(QPainter::Antialiasing);
    paint.translate(.5, .5);
    for (const auto& stroke : strokes_) {
      paint.setPen(QPen(
          stroke.erase ? source_ : QBrush(QColor(65, 210, 235)),
          stroke.diameter,
          Qt::SolidLine,
          Qt::RoundCap,
          Qt::RoundJoin));
      if (stroke.points.size() == 1)
        paint.drawPoint(stroke.points.front());
      else
        paint.drawPolyline(stroke.points);
    }
    if (hover_.x() >= 0) {
      paint.setPen(QPen(Qt::white, 1));
      paint.drawEllipse(QPointF(hover_), diameter_ / 2., diameter_ / 2.);
    }
  }
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton)
      record(event, true);
  }
  void mouseMoveEvent(QMouseEvent* event) override {
    record(event, false);
  }
  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton) {
      record(event, false);
      drawing_ = false;
    }
  }
  void leaveEvent(QEvent*) override {
    hover_ = {-1, -1};
    drawing_ = false;
    update();
  }

 private:
  void record(QMouseEvent* event, bool start) {
    const QRectF area = target();
    const QPointF offset = event->localPos() - area.topLeft();
    if (!area.contains(event->localPos())) {
      leaveEvent(nullptr);
      return;
    }
    hover_ = {
        std::clamp(int(offset.x() * image_.width() / area.width()), 0, image_.width() - 1),
        std::clamp(int(offset.y() * image_.height() / area.height()), 0, image_.height() - 1)};
    if (points_ < 12000 && (start || drawing_)) {
      if (start) {
        strokes_.push_back({diameter_, erase_, {}});
        drawing_ = true;
      }
      auto& line = strokes_.back().points;
      if (line.empty() || line.back() != hover_) {
        line.push_back(hover_);
        ++points_;
        changed(true, points_ == 12000);
      }
    }
    update();
  }
  struct Stroke {
    int diameter;
    bool erase;
    QPolygon points;
  };
  QImage image_;
  QBrush source_;
  QVector<Stroke> strokes_;
  QPoint hover_{-1, -1};
  int diameter_{1}, points_{0};
  bool drawing_{false};
  bool erase_{false};
};

RinkMaskPaintDialog::RinkMaskPaintDialog(const QImage& preview, const QImage& source, QWidget* parent)
    : QDialog(parent, Qt::Window) {
  configure_preview_dialog_window(this);
  setWindowTitle("Edit rink mask");
  resize(1050, 650);
  auto* layout = new QVBoxLayout(this);
  auto* tools = new QHBoxLayout;
  auto* mode = new QComboBox;
  mode->addItems({"Paint", "Erase"});
  mode->setEnabled(!source.isNull());
  auto* modeLabel = new QLabel("Brush");
  modeLabel->setBuddy(mode);
  tools->addWidget(modeLabel);
  tools->addWidget(mode);
  auto* width = new QSpinBox;
  width->setRange(1, 128);
  width->setValue(std::clamp(preview.height() / 24, 1, 128));
  width->setSuffix(" px");
  auto* label = new QLabel("Brush diameter");
  label->setBuddy(width);
  tools->addWidget(label);
  tools->addWidget(width);
  auto* undo = new QPushButton(action_icon(ActionIcon::Undo), "Undo stroke");
  tools->addWidget(undo);
  undo->setEnabled(false);
  tools->addStretch();
  tools->addWidget(new PreviewDialogWindowSizeButton(this));
  layout->addLayout(tools);
  canvas_ = new RinkMaskCanvas(preview, source);
  canvas_->setDiameter(width->value());
  layout->addWidget(canvas_, 1);
  auto* bottom = new QHBoxLayout;
  const QString instructions = source.isNull() ? "Green shows detected ice. Click or drag to paint."
                                               : "Green shows detected ice. Click or drag to paint or erase.";
  auto* hint = new QLabel(instructions);
  hint->setWordWrap(true);
  bottom->addWidget(hint, 1);
  auto* keep = new QPushButton(action_icon(ActionIcon::Next), "Use detected mask");
  auto* save = new QPushButton(action_icon(ActionIcon::Apply), "Apply mask");
  save->setEnabled(false);
  auto* cancel = new QPushButton(action_icon(ActionIcon::Cancel), "Cancel calibration");
  bottom->addWidget(cancel);
  bottom->addWidget(keep);
  bottom->addWidget(save);
  layout->addLayout(bottom);
  canvas_->changed = [undo, save, hint, instructions](bool painted, bool full) {
    undo->setEnabled(painted);
    save->setEnabled(painted);
    hint->setText(full ? "Stroke limit reached. Apply the mask or undo a stroke." : instructions);
  };
  connect(mode, qOverload<int>(&QComboBox::currentIndexChanged), canvas_, [this](int index) {
    canvas_->setErase(index == 1);
  });
  connect(width, qOverload<int>(&QSpinBox::valueChanged), canvas_, &RinkMaskCanvas::setDiameter);
  connect(undo, &QPushButton::clicked, this, [this] {
    canvas_->undo();
    canvas_->changed(canvas_->response() != "keep\n", false);
  });
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(keep, &QPushButton::clicked, this, [this] { done(UseDetectedMask); });
  connect(save, &QPushButton::clicked, this, &QDialog::accept);
}

QByteArray RinkMaskPaintDialog::response() const {
  return canvas_->response();
}
