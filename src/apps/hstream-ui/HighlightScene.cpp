#include "src/apps/hstream-ui/HighlightScene.h"

#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>
#include <QtGui/QFont>
#include <QtGui/QImageReader>
#include <QtGui/QPainter>
#include <algorithm>
#include <cmath>

namespace hm::ui {
namespace {
constexpr int kWidth = 1920, kHeight = 1080;
bool fail(QString* error, const QString& message) {
  if (error)
    *error = message;
  return false;
}
QFont font(double height, int weight) {
  QFont f("DejaVu Sans");
  f.setPixelSize(std::max(8, static_cast<int>(height)));
  f.setWeight(static_cast<QFont::Weight>(weight));
  return f;
}
void fitted_text(QPainter& p, const QRectF& r, const QString& text, double size, int weight) {
  QFont f = font(size, weight);
  while (f.pixelSize() > 10 && QFontMetricsF(f).horizontalAdvance(text) > r.width())
    f.setPixelSize(f.pixelSize() - 1);
  p.setFont(f);
  p.drawText(r, Qt::AlignCenter | Qt::TextWordWrap, text);
}
bool logo(const QString& path, const QString& root, QImage* result, QString* error) {
  if (path.isEmpty())
    return true;
  const QString absolute = QDir(root).absoluteFilePath(path);
  QImageReader reader(absolute);
  reader.setAutoTransform(true);
  const QSize size = reader.size();
  if (!size.isValid() || size.width() > 8192 || size.height() > 8192 || qint64(size.width()) * size.height() > 32000000)
    return fail(error, "Logo must be a readable image of at most 32 megapixels: " + absolute);
  reader.setScaledSize(size.scaled(640, 640, Qt::KeepAspectRatio));
  *result = reader.read();
  if (result->isNull())
    return fail(error, "Could not read logo: " + absolute);
  return true;
}
} // namespace

std::optional<QPointF> HighlightAnchor(const HighlightAnnotation& a, qint64 ms) {
  if (ms < a.start_ms || ms >= a.end_ms || (a.blink && ((ms - a.start_ms) / a.blink_ms) % 2))
    return std::nullopt;
  if (a.motion == "fixed")
    return QPointF(a.x, a.y);
  if (a.positions.isEmpty())
    return std::nullopt;
  auto next = std::upper_bound(
      a.positions.begin(), a.positions.end(), ms, [](qint64 t, const auto& p) { return t < p.time_ms; });
  if (a.motion == "track") {
    if (next == a.positions.begin())
      return std::nullopt;
    const auto& p = *(next - 1);
    // Never bridge an occlusion, a reset, or a missing observation. The author
    // can supply manual keyframes or create another explicitly selected cue.
    if (ms - p.time_ms > 100)
      return std::nullopt;
    return QPointF(p.x + a.x, p.y + a.y);
  }
  if (next == a.positions.begin())
    return QPointF(next->x, next->y);
  const auto& before = *(next - 1);
  if (next == a.positions.end())
    return QPointF(before.x, before.y);
  const double t = double(ms - before.time_ms) / (next->time_ms - before.time_ms);
  return QPointF(before.x + (next->x - before.x) * t, before.y + (next->y - before.y) * t);
}

HighlightTexture RasterHighlightAnnotation(const HighlightAnnotation& a, QSize output_size) {
  if (!output_size.isValid())
    output_size = QSize(kWidth, kHeight);
  const QSize reference = output_size.scaled(kWidth, kHeight, Qt::KeepAspectRatio);
  const int width = std::max(1, reference.width()), height = std::max(1, reference.height());
  const QPointF end(a.dx * width, a.dy * height);
  const double stroke = a.thickness * height;
  const double head = std::max(stroke * 3.5, 12.0);
  QRectF bounds;
  if (a.kind == "text") {
    const QFont f = font(a.size * height, a.weight);
    bounds = QFontMetricsF(f).boundingRect(QRectF(0, 0, width, height), Qt::TextWordWrap, a.text);
    bounds.moveTopLeft(QPointF(0, 0));
  } else {
    bounds = QRectF(QPointF(0, 0), end).normalized().adjusted(-head, -head, head, head);
  }
  // One small static raster per cue. Large/offscreen shapes are clipped to a
  // bounded asset rather than allocating an unbounded image.
  bounds = bounds.intersected(QRectF(-width, -height, width * 2, height * 2));
  const QRect r = bounds.toAlignedRect();
  HighlightTexture tile;
  tile.reference_size = QSize(width, height);
  tile.origin = QPointF(double(r.x()) / width, double(r.y()) / height);
  tile.image = QImage(std::max(1, r.width()), std::max(1, r.height()), QImage::Format_RGBA8888);
  tile.image.fill(Qt::transparent);
  QPainter p(&tile.image);
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::TextAntialiasing);
  p.translate(-r.x(), -r.y());
  p.setPen(QPen(QColor(a.color), stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  if (a.kind == "text") {
    p.setFont(font(a.size * height, a.weight));
    p.setPen(QColor(a.color));
    p.drawText(QRectF(0, 0, width, height), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, a.text);
  } else if (a.kind == "box") {
    p.drawRect(QRectF(QPointF(0, 0), end).normalized());
  } else {
    p.drawLine(QPointF(0, 0), end);
    const double length = std::hypot(end.x(), end.y());
    if (length > 0) {
      const QPointF unit = end / length, normal(-unit.y(), unit.x());
      const double h = std::min(head, length * 0.6);
      p.setBrush(QColor(a.color));
      p.setPen(Qt::NoPen);
      p.drawPolygon(QPolygonF{end, end - unit * h + normal * h * 0.5, end - unit * h - normal * h * 0.5});
    }
  }
  return tile;
}

bool RasterHighlightCard(
    const HighlightCard& c,
    const QString& root,
    QImage* image,
    QString* error,
    QSize output_size) {
  HighlightInterval item;
  item.is_card = true;
  item.card = c;
  if (!NormalizeHighlightInterval(&item, error))
    return false;
  QImage a, b;
  if (c.matchup && (!logo(c.logo_a, root, &a, error) || !logo(c.logo_b, root, &b, error)))
    return false;
  if (!output_size.isValid())
    output_size = QSize(kWidth, kHeight);
  // Fit the reference layout uniformly into the output aspect. Bound the static
  // raster independently of export resolution; the GPU scales it with the frame.
  const QSize fitted = output_size.scaled(kWidth, kHeight, Qt::KeepAspectRatio);
  const QSize raster_size(std::max(1, fitted.width()), std::max(1, fitted.height()));
  *image = QImage(raster_size, QImage::Format_RGBA8888);
  image->fill(QColor(c.background));
  QPainter p(image);
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  const double scale = std::min(double(raster_size.width()) / kWidth, double(raster_size.height()) / kHeight);
  p.translate((raster_size.width() - kWidth * scale) / 2, (raster_size.height() - kHeight * scale) / 2);
  p.scale(scale, scale);
  p.setPen(QColor(c.color));
  if (!c.matchup) {
    fitted_text(p, QRectF(140, 140, 1640, 800), c.heading, c.size * kHeight, c.weight);
    return true;
  }
  p.fillRect(QRectF(120, 100, 1680, 5), QColor(c.color));
  fitted_text(p, QRectF(150, 150, 1620, 130), c.heading, c.size * kHeight * 0.75, c.weight);
  auto team = [&](const QImage& logo, const QString& name, int x) {
    if (!logo.isNull()) {
      const QSize fitted = logo.size().scaled(300, 300, Qt::KeepAspectRatio);
      p.drawImage(
          QRectF(x + (700 - fitted.width()) / 2, 330 + (300 - fitted.height()) / 2, fitted.width(), fitted.height()),
          logo);
    }
    fitted_text(p, QRectF(x, 660, 700, 160), name, c.size * kHeight, c.weight);
  };
  team(a, c.team_a, 150);
  team(b, c.team_b, 1070);
  fitted_text(p, QRectF(870, 420, 180, 160), "VS", c.size * kHeight * 0.65, 900);
  fitted_text(p, QRectF(200, 890, 1520, 80), c.date, c.size * kHeight * 0.5, 500);
  return true;
}

bool ImportHighlightLogo(const QString& source, const QString& game, QString* path, QString* error) {
  QImage image;
  if (!logo(source, game, &image, error) || image.isNull())
    return false;
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  if (!image.save(&buffer, "PNG"))
    return fail(error, "Could not encode logo");
  const QString relative = "highlight-assets/" +
      QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) + ".png";
  if (!QDir(game).mkpath("highlight-assets"))
    return fail(error, "Could not create highlight-assets");
  QSaveFile file(QDir(game).filePath(relative));
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    return fail(error, "Could not save logo: " + file.errorString());
  *path = relative;
  return true;
}
} // namespace hm::ui
