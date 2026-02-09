#include "MainWindow.h"

#include <gst/gst.h>
#include <QtCore/QCommandLineParser>
#include <QtWidgets/QApplication>

int main(int argc, char* argv[]) {
  // Initialize GStreamer
  gst_init(&argc, &argv);

  // Initialize Qt
  QApplication app(argc, argv);
  app.setApplicationName("GstPipelineStudio");
  app.setApplicationVersion("0.1.0");

  // Parse command line arguments
  QCommandLineParser parser;
  parser.setApplicationDescription("GStreamer Pipeline Studio");
  parser.addHelpOption();
  parser.addVersionOption();

  QCommandLineOption pipelineOption(QStringList() << "p" << "pipeline", "Load pipeline from string", "pipeline-string");
  parser.addOption(pipelineOption);

  QCommandLineOption fileOption(QStringList() << "f" << "file", "Load pipeline from file", "filename");
  parser.addOption(fileOption);

  parser.process(app);

  // Create and show the main window
  MainWindow mainWindow;

  // Load pipeline if specified
  if (parser.isSet(pipelineOption)) {
    mainWindow.loadPipelineFromString(parser.value(pipelineOption));
  } else if (parser.isSet(fileOption)) {
    mainWindow.loadPipelineFromFile(parser.value(fileOption));
  }

  mainWindow.show();

  return app.exec();
}
