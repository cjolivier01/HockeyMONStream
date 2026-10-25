#include "src/apps/hstream-ui/ActionIcons.h"

#include <QtCore/QBuffer>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtWidgets/QApplication>

#include <iostream>

int main(int argc, char** argv) {
  QApplication application(argc, argv);
  // Freeze the deployed Linux Qt style; do not inherit a developer's theme.
  QApplication::setStyle("Fusion");
  install_button_icon_style();
  QJsonObject icons;
  for (const auto& spec : kActionIconSpecs) {
    QJsonObject sizes;
    for (int size : {16, 32}) {
      QByteArray png;
      QBuffer buffer(&png);
      buffer.open(QIODevice::WriteOnly);
      if (!action_icon(spec.icon).pixmap(size, size).save(&buffer, "PNG")) {
        std::cerr << "Cannot export icon " << spec.name << '\n';
        return 1;
      }
      sizes.insert(QString::number(size), QString::fromLatin1(png.toBase64()));
    }
    icons.insert(QString::fromLatin1(spec.name), sizes);
  }
  QJsonObject document;
  document.insert("style", "Fusion");
  document.insert("qt_version", qVersion());
  document.insert("icons", icons);
  std::cout << QJsonDocument(document).toJson(QJsonDocument::Compact).constData() << '\n';
  return 0;
}
