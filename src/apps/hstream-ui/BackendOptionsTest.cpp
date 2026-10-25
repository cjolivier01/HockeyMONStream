#include "src/apps/hstream-ui/BackendOptions.h"
#include "src/apps/hstream-ui/BackendPreview.h"
#include "src/apps/hstream-ui/NativePreviewSurface.h"

#include <QtCore/QFile>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtGui/QImage>
#include <QtWidgets/QApplication>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <typename F>
void rejects(F function, const char* message) {
  try {
    function();
  } catch (const std::runtime_error&) {
    return;
  }
  throw std::runtime_error(message);
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    hm::ui::BackendOptions{}.validate();
    rejects([] { hm::ui::BackendOptions{"hockeymon", "relative"}.validate(); }, "Reject relative backend runners");
    rejects([] { hm::ui::BackendOptions{"unknown"}.validate(); }, "Reject unknown backends");
    rejects([] { hm::ui::BackendOptions{"hockeymon"}.validate(); }, "Require explicit HockeyMON runner");
    const QJsonObject contract{
        {"protocol", "hstream-ui"}, {"version", 1}, {"backend", "hockeymon"}, {"preview", "jpeg-files"}};
    hm::ui::BackendOptions::validateHandshake(contract);
    auto incompatible = contract;
    incompatible["version"] = 2;
    rejects([&] { hm::ui::BackendOptions::validateHandshake(incompatible); }, "Reject protocol version mismatch");
    incompatible = contract;
    incompatible["preview"] = "native";
    rejects(
        [&] { hm::ui::BackendOptions::validateHandshake(incompatible); }, "Reject unimplemented preview transports");
    QTemporaryDir directory;
    require(directory.isValid(), "Temporary fixture directory");
    const QString runner = directory.filePath("hm-cli");
    QFile script(runner);
    require(script.open(QIODevice::WriteOnly), "Write handshake fixture");
    script.write(
        "#!/bin/sh\nprintf '%s\\n' '{\"protocol\":\"hstream-ui\",\"version\":1,\"backend\":\"hockeymon\",\"preview\":\"jpeg-files\"}'\n");
    script.close();
    require(
        script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "Executable handshake fixture");
    hm::ui::BackendOptions{"hockeymon", runner}.verifyRunner();

    const QString image_path = directory.filePath("preview-final.jpg");
    QImage source(320, 180, QImage::Format_RGB32);
    source.fill(Qt::red);
    require(source.save(image_path, "JPEG"), "Write preview fixture");
    require(hm::ui::BackendPreview::readImage(image_path).size() == source.size(), "Read actual JPEG preview");
    require(QFile::link(image_path, directory.filePath("linked.jpg")), "Create preview link fixture");
    rejects([&] { hm::ui::BackendPreview::readImage(directory.filePath("linked.jpg")); }, "Reject preview symlinks");
    QImage oversized(4097, 1, QImage::Format_RGB32);
    oversized.fill(Qt::black);
    require(oversized.save(directory.filePath("large.jpg"), "JPEG"), "Write oversized preview fixture");
    rejects(
        [&] { hm::ui::BackendPreview::readImage(directory.filePath("large.jpg")); }, "Bound dimensions before decode");

    hm::ui::BackendPreview preview;
    int received = 0;
    preview.frameReady = [&](const QString& channel, const QImage& frame) {
      require(channel == "program" && frame.size() == source.size(), "Correct preview channel and frame");
      ++received;
    };
    preview.failed = [](const QString& message) { throw std::runtime_error(message.toStdString()); };
    preview.start(directory.path());
    preview.setChannel("none");
    preview.poll();
    require(received == 0, "Disabled preview performs no decode");
    preview.setChannel("program");
    preview.poll();
    preview.poll();
    require(received == 1, "Decode each published frame at most once");
    preview.stop();
    preview.poll();
    require(received == 1, "Stopping removes the old session");
    require(hm::ui::BackendPreview::fileName("../../secret").isEmpty(), "Channels cannot name arbitrary paths");

    hm::ui::NativePreviewSurface surface;
    surface.setAttribute(Qt::WA_PaintOnScreen);
    surface.setAttribute(Qt::WA_NoSystemBackground);
    surface.resize(320, 180);
    surface.setImageMode(true);
    surface.setImage(source);
    require(!surface.testAttribute(Qt::WA_PaintOnScreen), "Python preview lets Qt own pixels");
    surface.show();
    app.processEvents();
    require(surface.grab().toImage().pixelColor(100, 100).red() > 240, "Shared surface renders Python frame");
    surface.setImageMode(false);
    require(
        surface.testAttribute(Qt::WA_PaintOnScreen) && surface.testAttribute(Qt::WA_NoSystemBackground),
        "Switching back restores native renderer ownership");
    std::cout << "Backend handshake, bounded preview and renderer ownership checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
