#include "src/apps/hstream-ui/RinkWarpEditor.h"
#include "src/apps/rink-warp-editor/RinkWarpAssets.h"

#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtGui/QImageReader>

#include <cmath>
#include <stdexcept>

namespace {
struct ImageSnapshot {
  QString data;
  QSize size;
};

ImageSnapshot read_image(const QString& path) {
  QFile file(path);
  constexpr qint64 kMaximumFileBytes = 256LL * 1024 * 1024;
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > kMaximumFileBytes)
    throw std::runtime_error("Could not read the image, or it exceeds the 256 MiB still-image limit.");
  QByteArray bytes = file.read(kMaximumFileBytes + 1);
  if (bytes.size() > kMaximumFileBytes || file.error() != QFileDevice::NoError)
    throw std::runtime_error("Could not finish reading the still image.");
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer);
  const QSize size = reader.size();
  const QByteArray format = reader.format();
  if ((format != "png" && format != "jpeg" && format != "webp") || size.width() < 2 || size.height() < 2 ||
      size.width() >= 32767 || size.height() >= 32767 || qint64(size.width()) * size.height() > 96LL * 1024 * 1024)
    throw std::runtime_error("Choose a PNG, JPEG or WebP still below 32767 pixels per side and 96 megapixels.");
  return {
      QString("data:image/%1;base64,%2").arg(QString::fromLatin1(format), QString::fromLatin1(bytes.toBase64())), size};
}

QJsonObject defaults(const YAML::Node& baseline) {
  const auto source = baseline["hstream_ui"]["rink_warp"];
  if (!source.IsMap())
    throw std::runtime_error("The baseline is missing hstream_ui.rink_warp.");
  QJsonObject result;
  for (const char* key : {"target_rows", "horizontal_crop"}) {
    const auto values = source[key];
    if (!values.IsSequence() || values.size() != 2)
      throw std::runtime_error("Rink warp defaults require two target rows and two crop bounds.");
    const double first = values[0].as<double>(), second = values[1].as<double>();
    if (!std::isfinite(first) || !std::isfinite(second) || first < 0 || second > 1 || second - first < 1e-4 ||
        (QString(key) == "target_rows" && (first < 1e-4 || second > 1 - 1e-4)))
      throw std::runtime_error("Rink warp baseline target rows or crop bounds are invalid.");
    result[key] = QJsonArray{first, second};
  }
  for (const char* key : {"strength", "aspect_balance"}) {
    const double value = source[key].as<double>();
    if (!std::isfinite(value) || value < 0 || value > 1)
      throw std::runtime_error("Rink warp baseline strengths must lie in [0, 1].");
    result[key] = value;
  }
  const int count = source["guide_count"].as<int>(), degree = source["fit_degree"].as<int>();
  const int dimension = source["preview_max_dimension"].as<int>();
  if (count < 3 || count > 129 || (degree != 2 && degree != 4 && degree != 6) || dimension < 256 || dimension > 16384)
    throw std::runtime_error("Rink warp baseline fitting or preview limits are invalid.");
  const auto active = source["active_guide"].as<std::string>();
  if (active != "upper" && active != "lower")
    throw std::runtime_error("Rink warp baseline active_guide must be upper or lower.");
  result["active_guide"] = QString::fromStdString(active);
  result["guide_count"] = count;
  result["fit_degree"] = degree;
  result["preview_max_dimension"] = dimension;
  for (const char* key : {"symmetric", "show_guides", "show_mask"})
    result[key] = source[key].as<bool>();
  return result;
}
} // namespace

QString WriteRinkWarpEditor(
    const QString& image_path,
    const QString& mask_path,
    const YAML::Node& baseline,
    const QString& cache_directory) {
  QJsonObject payload{{"defaults", defaults(baseline)}, {"title", QFileInfo(image_path).fileName()}};
  const auto image = read_image(image_path);
  payload["image"] = image.data;
  if (!mask_path.isEmpty()) {
    try {
      const auto mask = read_image(mask_path);
      if (mask.size != image.size)
        throw std::runtime_error("The saved ice mask has different dimensions from this panorama.");
      payload["mask"] = mask.data;
    } catch (const std::exception& error) {
      payload["notice"] = QString::fromUtf8(error.what()) + " Use Open ice mask to choose a matching mask.";
    }
  }
  QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
  json.replace("<", "\\u003c");
  QByteArray html(kRinkWarpEditorHtml);
  html.replace("@@WARP_BOOTSTRAP@@", json);
  if (cache_directory.isEmpty() || !QDir().mkpath(cache_directory))
    throw std::runtime_error("Could not create the private rink-warp cache directory.");
  const auto directory_permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
  if (!QFile::setPermissions(cache_directory, directory_permissions))
    throw std::runtime_error("Could not protect the private rink-warp cache directory.");
  const QString hash = QString::fromLatin1(QCryptographicHash::hash(html, QCryptographicHash::Sha256).toHex());
  const QString path = QDir(cache_directory).filePath(hash + ".html");
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
      file.write(html) != html.size() || !file.commit())
    throw std::runtime_error("Could not write the private still-editor snapshot.");
  return path;
}
