#include "src/apps/hmstream-ui/HmStreamWindow.h"

#include <QtWidgets/QApplication>

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  HmStreamWindow window;
  window.show();
  return app.exec();
}
