#include "src/apps/hstream-ui/CameraExperimentPreview.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>

#include <gst/gst.h>

#include <iostream>

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  gst_init(nullptr, nullptr);
  // Touch the registry first, as ordinary startup does. An installed factory
  // must not prevent loading the matching plugin shipped with this executable.
  if (auto* cached = gst_element_factory_find("playcropper"))
    gst_object_unref(cached);
  std::string error;
  if (!LoadCameraExperimentPlugin(&error)) {
    std::cerr << error << '\n';
    return 1;
  }
  const QString expected = QFileInfo(
                               QCoreApplication::applicationFilePath() +
                               ".runfiles/kstream/src/gst-plugins/gst-videoprep/libnvdsgst_videoprep.so")
                               .canonicalFilePath();
  GstElementFactory* factory = gst_element_factory_find("playcropper");
  GstPlugin* plugin = factory ? gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(factory)) : nullptr;
  const char* filename = plugin ? gst_plugin_get_filename(plugin) : nullptr;
  const QString actual = filename ? QFileInfo(QString::fromLocal8Bit(filename)).canonicalFilePath() : QString();
  if (plugin)
    gst_object_unref(plugin);
  if (factory)
    gst_object_unref(factory);
  if (expected.isEmpty() || actual != expected) {
    std::cerr << "Camera plugin came from outside the executable's runfiles: " << actual.toStdString() << '\n';
    return 1;
  }
  std::cout << "Camera plugin loaded from " << actual.toStdString() << '\n';
  return 0;
}
