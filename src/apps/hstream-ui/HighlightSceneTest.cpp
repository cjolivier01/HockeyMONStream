#include "src/apps/hstream-ui/HighlightScene.h"
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <iostream>
#include <limits>
using namespace hm::ui;
namespace {
bool expect(bool c, const char* s) {
  if (!c)
    std::cerr << s << '\n';
  return c;
}
QRect red_bounds(const QImage& image) {
  QRect bounds;
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      if (pixel.red() > 200 && pixel.green() < 50 && pixel.blue() < 50)
        bounds |= QRect(x, y, 1, 1);
    }
  return bounds;
}
} // namespace
int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  bool ok = true;
  QString error;
  HighlightAnnotation a;
  a.start_ms = 10000;
  a.end_ms = 12000;
  ok &= expect(
      !HighlightAnchor(a, 9999) && HighlightAnchor(a, 10000) && !HighlightAnchor(a, 10250) &&
          HighlightAnchor(a, 10500) && !HighlightAnchor(a, 12000),
      "Cue blink and exclusive end boundaries");
  a.blink = false;
  a.motion = "keyframes";
  a.positions = {{10000, .2, .3}, {11000, .6, .7}};
  const auto midpoint = HighlightAnchor(a, 10500);
  ok &= expect(
      midpoint && std::abs(midpoint->x() - .4) < 1e-6 && std::abs(midpoint->y() - .5) < 1e-6,
      "Manual detail keyframes interpolate");
  a.motion = "track";
  a.x = 0;
  a.y = -.1;
  a.positions = {{10000, .2, .3}, {10033, .3, .4}, {11000, .6, .7}};
  ok &= expect(
      HighlightAnchor(a, 10060) && !HighlightAnchor(a, 10134) && !HighlightAnchor(a, 10500) &&
          HighlightAnchor(a, 11000),
      "Recorded gaps suppress cues");
  a.motion = "fixed";
  a.text = "Holding penalty";
  a.kind = "text";
  const auto tile = RasterHighlightAnnotation(a);
  int colored = 0;
  for (int y = 0; y < tile.image.height(); ++y)
    for (int x = 0; x < tile.image.width(); ++x)
      if (tile.image.pixelColor(x, y).alpha() > 0)
        ++colored;
  ok &= expect(colored > 100 && tile.image.width() < 1920, "Text raster is visible and bounded");
  HighlightInterval card;
  card.is_card = true;
  card.card.matchup = true;
  card.card.team_a = "Home team";
  card.card.team_b = "Visitors";
  ok &= expect(!NormalizeHighlightInterval(&card, &error), "Unknown game date cannot default to today");
  card.card.date = "2026-02-29";
  ok &= expect(!NormalizeHighlightInterval(&card, &error), "Invalid calendar date rejected");
  card.card.date = "2026-10-08";
  QTemporaryDir dir;
  QImage logo(80, 40, QImage::Format_RGBA8888);
  logo.fill(Qt::red);
  const QString source = dir.filePath("team.png");
  logo.save(source);
  QString stored;
  ok &= expect(ImportHighlightLogo(source, dir.path(), &stored, &error), "Logo copied to content-addressed game asset");
  QFile::remove(source);
  card.card.logo_a = stored;
  QImage image;
  ok &= expect(
      RasterHighlightCard(card.card, dir.path(), &image, &error) && image.size() == QSize(1920, 1080),
      "Matchup survives original logo removal");
  for (const QSize output : {QSize(4096, 1024), QSize(1080, 1920)}) {
    ok &= expect(RasterHighlightCard(card.card, dir.path(), &image, &error, output), "Aspect-aware card rendering");
    const QRect bounds = red_bounds(image);
    ok &= expect(
        !bounds.isEmpty() && std::abs(double(bounds.width()) / bounds.height() - 2.0) < .05,
        "Wide and portrait cards preserve retained logo proportions");
  }
  a.text = "WHISTLE!";
  a.size = .1;
  const auto wide_text = RasterHighlightAnnotation(a, QSize(4096, 1024));
  a.size *= 480.0 / 1080;
  const auto same_height_text = RasterHighlightAnnotation(a);
  ok &= expect(
      wide_text.image.size() == same_height_text.image.size() && wide_text.reference_size == QSize(1920, 480),
      "Panorama cue typography uses image height and retains glyph proportions");
  card.card.logo_b = "missing.png";
  ok &= expect(
      !RasterHighlightCard(card.card, dir.path(), &image, &error), "Missing logo must fail instead of disappearing");
  card.card.logo_b.clear();
  HighlightPlan plan;
  plan.archive_path = "/saved/archive.mp4";
  HighlightInterval clip;
  clip.event_mode = false;
  clip.start_ms = 10000;
  clip.end_ms = 13000;
  clip.annotations = {a};
  plan.intervals = {card, clip, card};
  const QString file = dir.filePath("highlights.json");
  HighlightPlan loaded;
  ok &= expect(
      SaveHighlightPlan(file, plan, &error) && LoadHighlightPlan(file, &loaded, &error) &&
          loaded.archive_path == plan.archive_path && loaded.intervals.size() == 3 &&
          loaded.intervals[0].card.date == "2026-10-08" && loaded.intervals[1].annotations.size() == 1 &&
          loaded.intervals[1].annotations[0].start_ms == 10000,
      "Mixed reel with cues and assets round trips");
  loaded.intervals[0].card.duration_ms += 1000;
  loaded.intervals.swapItemsAt(0, 1);
  loaded.intervals[0].start_ms = 10500;
  ok &=
      expect(loaded.intervals[0].annotations[0].start_ms == 10000, "Cards, reorder and trim preserve source cue times");
  a.x = std::numeric_limits<double>::quiet_NaN();
  ok &= expect(!ValidateHighlightAnnotation(a, &error), "Nonfinite positions rejected");
  return ok ? 0 : 1;
}
