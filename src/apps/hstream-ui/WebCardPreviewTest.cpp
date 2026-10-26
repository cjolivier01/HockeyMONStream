#include "src/apps/hstream-ui/HighlightScene.h"
#include "src/apps/hstream-ui/WebJobSpec.h"

#include <sys/stat.h>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <iostream>

namespace {
bool command(const QString& program, const QStringList& arguments) {
  QProcess child;
  child.start(program, arguments);
  if (!child.waitForFinished(45000) || child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0) {
    std::cerr << child.readAllStandardError().constData() << '\n';
    child.kill();
    child.waitForFinished();
    return false;
  }
  return true;
}
QJsonObject identity(const QString& path) {
  struct stat info{};
  if (::stat(path.toLocal8Bit().constData(), &info) != 0)
    throw std::runtime_error("Cannot stat archive fixture");
  return {
      {"dev", QString::number(info.st_dev)},
      {"ino", QString::number(info.st_ino)},
      {"size", QString::number(info.st_size)},
      {"mtime_ns", QString::number(qint64(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec)},
      {"ctime_ns", QString::number(qint64(info.st_ctim.tv_sec) * 1000000000 + info.st_ctim.tv_nsec)}};
}
} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  if (argc != 2)
    return 1;
  const auto binary = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
  QTemporaryDir temporary;
  if (!temporary.isValid())
    return 1;
  try {
    int index = 0;
    for (const auto& geometry : {QSize(320, 240), QSize(1280, 320), QSize(360, 640)}) {
      const auto archive = temporary.filePath(QString("archive-%1.mp4").arg(index));
      if (!command(
              "ffmpeg",
              {"-hide_banner",
               "-loglevel",
               "error",
               "-f",
               "lavfi",
               "-i",
               QString("color=c=blue:s=%1x%2:r=1:d=1").arg(geometry.width()).arg(geometry.height()),
               "-c:v",
               "libx264",
               "-pix_fmt",
               "yuv420p",
               archive}))
        return 1;
      for (const auto& format : {"PNG", "JPEG", "WEBP"}) {
        const auto root = temporary.filePath(QString("case-%1-%2").arg(index).arg(format));
        QDir().mkpath(root + "/game");
        QImage logo(80, 40, QImage::Format_RGBA8888);
        logo.fill(Qt::red);
        if (!logo.save(root + "/team", format))
          return 1;
        hm::ui::HighlightInterval card;
        card.is_card = true;
        card.card.matchup = index != 0;
        card.card.heading = "An unsaved authored title";
        card.card.team_a = "Home team";
        card.card.team_b = "Visitors";
        card.card.date = "2026-10-25";
        card.card.logo_a = "team";
        card.card.background = "#213141";
        card.card.color = "#fff080";
        card.card.size = .09;
        card.card.weight = 600;
        hm::ui::HighlightPlan plan;
        plan.intervals = {card};
        QString error;
        if (!hm::ui::SaveHighlightPlan(root + "/source-plan.json", plan, &error)) {
          std::cerr << error.toStdString() << '\n';
          return 1;
        }
        const auto frozen = identity(archive);
        const QJsonObject request{
            {"schema", 1},
            {"run_id", "card-preview-test"},
            {"operation", "highlight-card-preview"},
            {"root", root},
            {"game_dir", "game"},
            {"authorized_media", QJsonArray{archive}},
            {"runtime", QJsonObject{{"archive_probe", QJsonObject{{"path", archive}, {"identity", frozen}}}}},
            {"payload", QJsonObject{{"plan", hm::web::ReadObject((root + "/source-plan.json").toStdString())}}}};
        const auto path = root + "/operation-request.json";
        hm::web::WriteObject(path.toStdString(), request);
        if (!command(binary, {"--request", path}))
          return 1;
        QImage expected;
        if (!hm::ui::RasterHighlightCard(card.card, root, &expected, &error, geometry))
          return 1;
        const QImage actual(root + "/highlight-card.png");
        if (actual.convertToFormat(QImage::Format_RGBA8888) != expected.convertToFormat(QImage::Format_RGBA8888) ||
            identity(archive) != frozen) {
          std::cerr << "Web preview differs from the Qt card renderer or changed its archive\n";
          return 1;
        }
        const auto receipt = hm::web::ReadObject((root + "/operation-result.json").toStdString());
        const auto result = receipt.value("result").toObject();
        if (receipt.value("state") != "complete" || receipt.value("artifacts").toArray().size() != 2 ||
            result.value("source_width").toInt() != geometry.width() ||
            result.value("source_height").toInt() != geometry.height() ||
            result.value("width").toInt() != expected.width() || result.value("height").toInt() != expected.height())
          return 1;
      }
      ++index;
    }
    const auto root = temporary.filePath("without-archive");
    QDir().mkpath(root + "/game");
    hm::ui::HighlightInterval card;
    card.is_card = true;
    card.card.heading = "A card before choosing an archive";
    hm::ui::HighlightPlan plan;
    plan.intervals = {card};
    QString error;
    if (!hm::ui::SaveHighlightPlan(root + "/source-plan.json", plan, &error))
      return 1;
    const QJsonObject request{
        {"schema", 1},
        {"run_id", "no-archive-card-test"},
        {"operation", "highlight-card-preview"},
        {"root", root},
        {"game_dir", "game"},
        {"payload", QJsonObject{{"plan", hm::web::ReadObject((root + "/source-plan.json").toStdString())}}}};
    hm::web::WriteObject((root + "/operation-request.json").toStdString(), request);
    if (!command(binary, {"--request", root + "/operation-request.json"}))
      return 1;
    QImage expected;
    if (!hm::ui::RasterHighlightCard(card.card, root, &expected, &error, QSize()))
      return 1;
    const QImage actual(root + "/highlight-card.png");
    const auto result = hm::web::ReadObject((root + "/operation-result.json").toStdString()).value("result").toObject();
    if (actual.convertToFormat(QImage::Format_RGBA8888) != expected.convertToFormat(QImage::Format_RGBA8888) ||
        result.value("source_width").toInt() != expected.width() ||
        result.value("source_height").toInt() != expected.height()) {
      std::cerr << "Card preview without an archive differs from the Qt editor\n";
      return 1;
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
