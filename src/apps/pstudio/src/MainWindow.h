#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "ElementLibrary.h"
#include "GstPipelineModel.h"
#include "PipelineEditor.h"

#include <gst/gst.h>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QTextEdit>
#include <QtCore/QTimer>

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow();

  bool loadPipelineFromString(const QString& pipelineString);
  bool loadPipelineFromFile(const QString& filename);

 private slots:
  void newPipeline();
  void openPipeline();
  bool savePipeline(); // Changed from void to bool
  bool savePipelineAs(); // Changed from void to bool
  void runPipeline();
  void stopPipeline();
  void pausePipeline();
  void buildPipeline();
  void showPipelineDot();
  void showElementProperties(const QString& elementName);
  void onPipelineStateChanged(GstState state);
  void onPipelineError(const QString& message);
  void updatePipelineStatusMessage();

 private:
  void createActions();
  void createMenus();
  void createToolbars();
  void createDockWindows();
  void createStatusBar();
  void setupConnections();
  bool saveCurrentPipeline(const QString& filename);
  void setCurrentFile(const QString& filename);
  void updateWindowTitle();

  // UI Components
  PipelineEditor* m_pipelineEditor{nullptr};
  ElementLibrary* m_elementLibrary{nullptr};
  QTextEdit* m_logViewer{nullptr};
  QTextEdit* m_propertyEditor{nullptr};
  QDockWidget* m_elementDock{nullptr};
  QDockWidget* m_logDock{nullptr};
  QDockWidget* m_propertyDock{nullptr};
  QLabel* m_statusLabel{nullptr};

  // Pipeline Management
  GstPipelineModel* m_pipelineModel{nullptr};
  QString m_currentFilename;
  bool m_isModified{false};
  QTimer m_statusTimer;
};

#endif // MAINWINDOW_H
