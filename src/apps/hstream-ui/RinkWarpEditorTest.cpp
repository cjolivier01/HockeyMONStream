#include "src/apps/hstream-ui/RinkWarpEditor.h"
#include "hstream/src/libs/common/BaselineConfig.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
QByteArray read(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Read fixture");
  return file.readAll();
}
QJsonObject payload(const QString& path) {
  const auto bytes = read(path);
  const QByteArray marker = "id=\"warp-bootstrap\">";
  const qsizetype first = bytes.indexOf(marker) + marker.size();
  const qsizetype last = bytes.indexOf("</script>", first);
  require(first >= marker.size() && last > first, "Self-contained editor embeds its bootstrap");
  QJsonParseError error;
  const auto json = QJsonDocument::fromJson(bytes.mid(first, last - first), &error);
  require(error.error == QJsonParseError::NoError, "Bootstrap JSON must be valid");
  require(!bytes.mid(first, last - first).contains('<'), "Bootstrap must not inject markup");
  return json.object();
}
} // namespace
int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  try {
    const auto baseline = hm::baseline_config::load();
    require(baseline.ok(), "Load the canonical baseline");
    QTemporaryDir temp;
    const QString source = QDir(temp.path()).filePath("source<&>.png");
    const QString mask_path = QDir(temp.path()).filePath("mask.png");
    QImage image(321, 151, QImage::Format_RGB32);
    image.fill(Qt::cyan);
    require(image.save(source), "Write source fixture");
    image.fill(Qt::white);
    require(image.save(mask_path), "Write mask fixture");
    const auto source_before = read(source), mask_before = read(mask_path);
    const QString cache = QDir(temp.path()).filePath("cache");
    const QString path = WriteRinkWarpEditor(source, mask_path, baseline->values, cache);
    const auto boot = payload(path);
    require(boot["image"].toString().startsWith("data:image/png;base64,"), "Image is a frozen inline snapshot");
    require(boot["mask"].toString().startsWith("data:image/png;base64,"), "Matching mask is included");
    require(boot["title"].toString() == "source<&>.png", "Image name survives escaped JSON");
    require(
        boot["defaults"].toObject()["guide_count"].toInt() ==
            baseline->values["hstream_ui"]["rink_warp"]["guide_count"].as<int>(),
        "Defaults come from baseline");
    require(
        read(source) == source_before && read(mask_path) == mask_before, "Editor launch must not change image or mask");
    const auto permissions = QFile::permissions(path);
    require(
        !(permissions & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)),
        "Snapshot is private");
    require(
        WriteRinkWarpEditor(source, mask_path, baseline->values, cache) == path,
        "Repeated launch reuses content-addressed cache");
    QImage mismatch(123, 45, QImage::Format_RGB32);
    mismatch.fill(Qt::white);
    require(mismatch.save(mask_path), "Write mismatched mask");
    const auto no_mask = payload(WriteRinkWarpEditor(source, mask_path, baseline->values, cache));
    require(
        !no_mask.contains("mask") && no_mask["notice"].toString().contains("different dimensions"),
        "Stale mask does not strand the image editor");
    auto invalid = YAML::Clone(baseline->values);
    invalid["hstream_ui"].remove("rink_warp");
    bool rejected = false;
    try {
      WriteRinkWarpEditor(source, {}, invalid, cache);
    } catch (const std::exception&) {
      rejected = true;
    }
    require(rejected, "Missing defaults fail instead of silently inventing them");
    std::cout << "Rink warp editor snapshots passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
