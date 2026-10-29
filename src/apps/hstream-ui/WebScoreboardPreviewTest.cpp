#include "src/apps/hstream-ui/WebScoreboardPreview.h"
#include "src/apps/hstream-ui/CalibrationImage.h"

#include <png.h>
#include <sys/stat.h>
#include <unistd.h>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImageReader>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}

QJsonObject identity(const QString& path) {
  struct stat status{};
  require(::stat(QFile::encodeName(path).constData(), &status) == 0, "Cannot inspect fixture");
  return {
      {"device", QString::number(static_cast<qulonglong>(status.st_dev))},
      {"inode", QString::number(static_cast<qulonglong>(status.st_ino))},
      {"size", QString::number(status.st_size)},
      {"mtime_ns", QString::number(qint64(status.st_mtim.tv_sec) * 1000000000LL + status.st_mtim.tv_nsec)},
      {"ctime_ns", QString::number(qint64(status.st_ctim.tv_sec) * 1000000000LL + status.st_ctim.tv_nsec)}};
}

void write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Cannot write fixture");
}

void fixture(const QString& path, int width = 640, int height = 360, bool interlaced = false) {
  // Row-only generation also exercises a 194.9 MP panorama without a source bitmap.
  FILE* file = std::fopen(QFile::encodeName(path).constData(), "wb");
  require(file, "Cannot open PNG fixture");
  auto* png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  auto* info = png_create_info_struct(png);
  require(png && info && !setjmp(png_jmpbuf(png)), "Cannot write PNG fixture");
  png_init_io(png, file);
  png_set_IHDR(
      png,
      info,
      width,
      height,
      8,
      PNG_COLOR_TYPE_RGB,
      interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
      PNG_COMPRESSION_TYPE_DEFAULT,
      PNG_FILTER_TYPE_DEFAULT);
  png_set_compression_level(png, 1);
  png_write_info(png, info);
  const int passes = png_set_interlace_handling(png);
  std::vector<unsigned char> row(width * 3);
  for (int pass = 0; pass < passes; ++pass) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        row[x * 3] = x % 251;
        row[x * 3 + 1] = y % 253;
        row[x * 3 + 2] = 99;
      }
      png_write_row(png, row.data());
    }
  }
  png_write_end(png, info);
  png_destroy_write_struct(&png, &info);
  std::fclose(file);
}

QJsonObject request(const QTemporaryDir& root, const QString& token = QString(32, 'a')) {
  return {
      {"schema", "hstream-scoreboard-preview-request-v1"},
      {"token", token},
      {"root", root.path()},
      {"source", "game/s.png"},
      {"source_identity", identity(root.filePath("game/s.png"))}};
}

void rejected(const QJsonObject& input, const std::function<void()>& change = {}) {
  bool failed = false;
  try {
    hm::web::ScoreboardPreview(input, change);
  } catch (const std::exception&) {
    failed = true;
  }
  require(failed, "Invalid preview must fail closed");
  const QDir root(input.value("root").toString());
  require(
      root.entryList({"scoreboard-preview-*"}, QDir::Files).isEmpty(),
      "Failed preview must not publish image or receipt");
}

void test_failures() {
  QTemporaryDir root;
  require(QDir(root.path()).mkdir("game"), "Cannot create game");
  const auto source = root.filePath("game/s.png");
  fixture(source);
  const auto original = request(root);
  for (const auto& token : {QString("../bad"), QString(31, 'a'), QString(32, 'z')}) {
    auto input = original;
    input["token"] = token;
    rejected(input);
  }
  for (const auto& path : {"../s.png", "/game/s.png", "game/../game/s.png", "game//s.png", "game/panorama.tif"}) {
    auto input = original;
    input["source"] = path;
    rejected(input);
  }
  auto wrong = original;
  wrong["schema"] = "other";
  rejected(wrong);
  wrong = original;
  wrong["source_identity"] = QJsonObject{};
  rejected(wrong);
  write(source, "broken PNG");
  rejected(request(root));
  fixture(source);
  rejected(request(root), [&] { fixture(source, 320, 180); });
  fixture(source);
  rejected(request(root), [&] {
    require(QFile::rename(source, root.filePath("old.png")), "Cannot replace source");
    fixture(source);
  });
  require(QFile::remove(source) && QFile::link(root.filePath("old.png"), source), "Cannot create source symlink");
  rejected(request(root));
  require(QFile::remove(source), "Cannot retire source symlink");
  fixture(source);
  require(QDir(root.path()).rename("game", "original-game"), "Cannot rename game");
  require(QFile::link(root.filePath("original-game"), root.filePath("game")), "Cannot create directory symlink");
  rejected(request(root));
}

void test_success(int width, int height, bool interlaced = false) {
  QTemporaryDir root;
  require(QDir(root.path()).mkdir("game"), "Cannot create game");
  fixture(root.filePath("game/s.png"), width, height, interlaced);
  const auto input = request(root);
  const auto result = hm::web::ScoreboardPreview(input);
  const auto expected = scoreboardPreviewSize({width, height});
  require(result["source_size"] == QJsonArray{width, height}, "Receipt must retain native coordinates");
  require(result["preview_size"] == QJsonArray{expected.width(), expected.height()}, "Receipt must bind proxy pixels");
  require(result["source_identity"] == input["source_identity"], "Receipt must bind exact source identity");
  const auto image_path = root.filePath(result["image"].toString());
  require(result["image_identity"].toObject() == identity(image_path), "Receipt must bind final image identity");
  const QImage image(image_path);
  require(!image.isNull() && image.size() == expected, "Proxy must be decodable at the shared size");
  for (const auto& point :
       {QPoint(0, 0),
        QPoint(expected.width() - 1, expected.height() - 1),
        QPoint(expected.width() / 2, expected.height() / 2)}) {
    const auto x = (qint64(2 * point.x() + 1) * width) / (2 * expected.width());
    const auto y = (qint64(2 * point.y() + 1) * height) / (2 * expected.height());
    require(image.pixelColor(point) == QColor(x % 251, y % 253, 99), "Proxy must preserve Qt midpoint sampling");
  }
  QFile file(image_path);
  require(file.open(QIODevice::ReadOnly), "Cannot read published proxy");
  QCryptographicHash hash(QCryptographicHash::Sha256);
  require(
      hash.addData(&file) && result["sha256"] == QString::fromLatin1(hash.result().toHex()), "Proxy hash must match");
  QFile receipt(root.filePath("scoreboard-preview-" + input["token"].toString() + ".json"));
  require(
      receipt.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(receipt.readAll()).object() == result,
      "Atomic receipt must match returned result");
  bool duplicate_failed = false;
  try {
    hm::web::ScoreboardPreview(input);
  } catch (const std::exception&) {
    duplicate_failed = true;
  }
  require(
      duplicate_failed && identity(image_path) == result["image_identity"].toObject(),
      "An old token must never replace its published proxy");
}

void test_source_bounds() {
  QTemporaryDir root;
  QDir(root.path()).mkdir("game");
  const auto path = root.filePath("game/s.png");
  // Valid header/chunk CRC, rejected before row allocation by native source bounds.
  for (const auto& dimensions : {QSize(65536, 1), QSize(20000, 20000)}) {
    FILE* file = std::fopen(QFile::encodeName(path).constData(), "wb");
    auto* png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    auto* info = png_create_info_struct(png);
    require(file && png && info && !setjmp(png_jmpbuf(png)), "Cannot write source bound fixture");
    png_init_io(png, file);
    png_set_IHDR(
        png,
        info,
        dimensions.width(),
        dimensions.height(),
        8,
        PNG_COLOR_TYPE_RGB,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    png_destroy_write_struct(&png, &info);
    std::fclose(file);
    rejected(request(root));
  }
}
} // namespace

int main() {
  try {
    require(scoreboardPreviewSize({640, 360}) == QSize(640, 360), "Small Qt images must remain unchanged");
    require(scoreboardPreviewSize({65535, 1}) == QSize(8192, 1), "Qt side bound must remain unchanged");
    require(scoreboardPreviewSize({}).isEmpty(), "Invalid Qt geometry must remain empty");
    test_failures();
    test_source_bounds();
    test_success(640, 360);
    test_success(640, 360, true);
    test_success(20419, 9547);
    std::cout << "scoreboard preview checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
