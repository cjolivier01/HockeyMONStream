#include "src/apps/hstream-ui/UiDiagnostics.h"
#include "hstream/src/libs/common/ProcessDiagnostics.h"

#include <QtCore/QDebug>
#include <QtCore/QTemporaryDir>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

int main(int argc, char** argv) {
  QTemporaryDir directory;
  qputenv("HSTREAM_DIAGNOSTICS_DIR", directory.path().toUtf8());
  if (!hm::diagnostics::Initialize("ui-test", argv[0]))
    return 1;
  install_qt_diagnostics();
  QApplication app(argc, argv);
  UiDiagnostics diagnostics;
  QDialog dialog;
  dialog.setWindowTitle("Calibration test");
  QPushButton button("Save matches", &dialog);
  QLineEdit input(&dialog);
  input.setText("DO-NOT-LOG-TYPED-INPUT");
  dialog.show();
  QMouseEvent click(
      QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&button, &click);
  QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "DO-NOT-LOG-TYPED-INPUT");
  QCoreApplication::sendEvent(&input, &key);
  qWarning("diagnostic warning test");
  dialog.close();
  hm::diagnostics::Finish(0);
  const auto read = [](const std::string& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
  };
  const auto breadcrumbs = read(hm::diagnostics::Directory() + "/breadcrumbs.log");
  const auto events = read(hm::diagnostics::Directory() + "/events.log");
  if (breadcrumbs.find("Save matches") == std::string::npos ||
      breadcrumbs.find("Calibration test") == std::string::npos ||
      breadcrumbs.find("DO-NOT-LOG-TYPED-INPUT") != std::string::npos ||
      events.find("diagnostic warning test") == std::string::npos)
    return 1;
  std::cout << "UI diagnostics tests passed\n";
  return 0;
}
