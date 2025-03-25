#include "MainWindow.h"

#include <QtCore/QDebug>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtCore/QProcess>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStatusBar>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTextStream>
#include <QtWidgets/QToolBar>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      m_pipelineEditor(new PipelineEditor(this)),
      m_elementLibrary(new ElementLibrary(this)),
      m_logViewer(new QTextEdit(this)),
      m_propertyEditor(new QTextEdit(this)),
      m_pipelineModel(new GstPipelineModel(this)),
      m_isModified(false) {
  // Setup UI
  setCentralWidget(m_pipelineEditor);
  createActions();
  createMenus();
  createToolbars();
  createDockWindows();
  createStatusBar();
  setupConnections();

  // Setup log viewer
  m_logViewer->setReadOnly(true);

  // Setup property editor
  m_propertyEditor->setReadOnly(false);

  // Setup status update timer
  m_statusTimer.setInterval(1000); // 1 second
  connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::updatePipelineStatusMessage);

  resize(1200, 800);
  setCurrentFile("");
}

MainWindow::~MainWindow() {
  // Ensure pipeline is stopped and cleaned up
  stopPipeline();
}

bool MainWindow::loadPipelineFromString(const QString& pipelineString) {
  bool success = m_pipelineModel->loadFromString(pipelineString);
  if (success) {
    m_pipelineEditor->setPipelineModel(m_pipelineModel);
    m_isModified = false;
    m_logViewer->append("Pipeline loaded from string successfully");
  } else {
    m_logViewer->append("Failed to load pipeline from string: " + m_pipelineModel->lastError());
  }
  return success;
}

bool MainWindow::loadPipelineFromFile(const QString& filename) {
  if (filename.isEmpty())
    return false;

  QFile file(filename);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    m_logViewer->append("Failed to open file: " + filename);
    return false;
  }

  QTextStream in(&file);
  QString pipelineString = in.readAll();
  file.close();

  bool success = loadPipelineFromString(pipelineString);
  if (success) {
    setCurrentFile(filename);
  }

  return success;
}

void MainWindow::newPipeline() {
  if (m_isModified) {
    QMessageBox::StandardButton response = QMessageBox::question(
        this,
        "Unsaved Changes",
        "You have unsaved changes. Do you want to save them before creating a new pipeline?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (response == QMessageBox::Save) {
      if (!savePipeline())
        return; // User canceled save operation
    } else if (response == QMessageBox::Cancel) {
      return;
    }
  }

  m_pipelineModel->clear();
  m_pipelineEditor->setPipelineModel(m_pipelineModel);
  setCurrentFile("");
  m_isModified = false;
  m_logViewer->append("Created new pipeline");
}

void MainWindow::openPipeline() {
  if (m_isModified) {
    QMessageBox::StandardButton response = QMessageBox::question(
        this,
        "Unsaved Changes",
        "You have unsaved changes. Do you want to save them before opening another pipeline?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (response == QMessageBox::Save) {
      if (!savePipeline())
        return; // User canceled save operation
    } else if (response == QMessageBox::Cancel) {
      return;
    }
  }

  QString filename =
      QFileDialog::getOpenFileName(this, "Open Pipeline", QString(), "Pipeline Files (*.gst);;All Files (*)");

  if (!filename.isEmpty()) {
    loadPipelineFromFile(filename);
  }
}

bool MainWindow::savePipeline() {
  if (m_currentFilename.isEmpty()) {
    return savePipelineAs();
  } else {
    return saveCurrentPipeline(m_currentFilename);
  }
}

bool MainWindow::savePipelineAs() {
  QString filename =
      QFileDialog::getSaveFileName(this, "Save Pipeline", QString(), "Pipeline Files (*.gst);;All Files (*)");

  if (filename.isEmpty())
    return false;

  return saveCurrentPipeline(filename);
}

void MainWindow::runPipeline() {
  if (m_pipelineModel->state() == GST_STATE_PLAYING) {
    m_logViewer->append("Pipeline is already running");
    return;
  }

  // Try to build the pipeline first if not built
  if (!m_pipelineModel->isBuilt()) {
    if (!m_pipelineModel->buildPipeline()) {
      m_logViewer->append("Failed to build pipeline: " + m_pipelineModel->lastError());
      return;
    }
  }

  if (m_pipelineModel->setState(GST_STATE_PLAYING)) {
    m_logViewer->append("Pipeline started");
    m_statusTimer.start();
  } else {
    m_logViewer->append("Failed to start pipeline: " + m_pipelineModel->lastError());
  }
}

void MainWindow::stopPipeline() {
  if (m_pipelineModel->state() == GST_STATE_NULL) {
    m_logViewer->append("Pipeline is already stopped");
    return;
  }

  if (m_pipelineModel->setState(GST_STATE_NULL)) {
    m_logViewer->append("Pipeline stopped");
    m_statusTimer.stop();
    m_statusLabel->setText("Pipeline: Stopped");
  } else {
    m_logViewer->append("Failed to stop pipeline: " + m_pipelineModel->lastError());
  }
}

void MainWindow::pausePipeline() {
  if (m_pipelineModel->state() == GST_STATE_PAUSED) {
    m_logViewer->append("Pipeline is already paused");
    return;
  }

  if (m_pipelineModel->setState(GST_STATE_PAUSED)) {
    m_logViewer->append("Pipeline paused");
  } else {
    m_logViewer->append("Failed to pause pipeline: " + m_pipelineModel->lastError());
  }
}

void MainWindow::buildPipeline() {
  if (m_pipelineModel->buildPipeline()) {
    m_logViewer->append("Pipeline built successfully");
  } else {
    m_logViewer->append("Failed to build pipeline: " + m_pipelineModel->lastError());
  }
}

void MainWindow::showPipelineDot() {
  if (!m_pipelineModel->isBuilt()) {
    if (!m_pipelineModel->buildPipeline()) {
      m_logViewer->append("Failed to build pipeline for visualization: " + m_pipelineModel->lastError());
      return;
    }
  }

  // Generate DOT file
  QTemporaryFile dotFile;
  if (!dotFile.open()) {
    m_logViewer->append("Failed to create temporary file for DOT visualization");
    return;
  }

  QString dotFilePath = dotFile.fileName();
  dotFile.close(); // Close but don't remove

  // Get pipeline as DOT format
  QString dotString = m_pipelineModel->toDotFormat();
  QFile file(dotFilePath);
  if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    QTextStream out(&file);
    out << dotString;
    file.close();
  } else {
    m_logViewer->append("Failed to write DOT file");
    return;
  }

  // Generate PNG from DOT
  QTemporaryFile pngFile;
  if (!pngFile.open()) {
    m_logViewer->append("Failed to create temporary file for PNG visualization");
    return;
  }

  QString pngFilePath = pngFile.fileName() + ".png";
  pngFile.close();

  QProcess process;
  process.start("dot", QStringList() << "-Tpng" << "-o" << pngFilePath << dotFilePath);
  if (!process.waitForFinished(5000)) {
    m_logViewer->append("Timeout or error while generating pipeline visualization");
    return;
  }

  // Show the PNG image
  QProcess viewerProcess;

  // Try to use an available image viewer
  QString viewer;

#ifdef Q_OS_WIN
  viewer = "explorer";
#elif defined(Q_OS_MAC)
  viewer = "open";
#else
  // Try various Linux image viewers
  QStringList viewers = {"xdg-open", "eog", "gwenview", "feh"};
  for (const QString& v : viewers) {
    if (QProcess::startDetached(v, QStringList() << pngFilePath)) {
      viewer = v;
      break;
    }
  }
#endif

  if (!viewer.isEmpty()) {
    QProcess::startDetached(viewer, QStringList() << pngFilePath);
  } else {
    m_logViewer->append("Failed to open image viewer. Pipeline visualization saved to: " + pngFilePath);
  }
}

void MainWindow::showElementProperties(const QString& elementName) {
  if (elementName.isEmpty())
    return;

  QMap<QString, QString> properties = m_pipelineModel->getElementProperties(elementName);

  m_propertyEditor->clear();

  if (properties.isEmpty()) {
    m_propertyEditor->append("No properties found for element: " + elementName);
    return;
  }

  m_propertyEditor->append("Properties for element: " + elementName + "\n");

  for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
    m_propertyEditor->append(it.key() + ": " + it.value());
  }
}

void MainWindow::onPipelineStateChanged(GstState state) {
  QString stateStr;

  switch (state) {
    case GST_STATE_VOID_PENDING:
      stateStr = "Pending";
      break;
    case GST_STATE_NULL:
      stateStr = "Stopped";
      m_statusTimer.stop();
      break;
    case GST_STATE_READY:
      stateStr = "Ready";
      break;
    case GST_STATE_PAUSED:
      stateStr = "Paused";
      break;
    case GST_STATE_PLAYING:
      stateStr = "Playing";
      break;
    default:
      stateStr = "Unknown";
      break;
  }

  m_statusLabel->setText("Pipeline: " + stateStr);
  m_logViewer->append("Pipeline state changed to: " + stateStr);
}

void MainWindow::onPipelineError(const QString& message) {
  m_logViewer->append("Pipeline error: " + message);
  QMessageBox::critical(this, "Pipeline Error", message);
}

void MainWindow::updatePipelineStatusMessage() {
  if (!m_pipelineModel->isBuilt() || m_pipelineModel->state() != GST_STATE_PLAYING)
    return;

  GstClockTime position = m_pipelineModel->position();
  GstClockTime duration = m_pipelineModel->duration();

  QString posStr = "Position: " + QString::number(position / GST_SECOND) + "." +
      QString::number((position % GST_SECOND) / (GST_SECOND / 100), 10, 2).rightJustified(2, '0') + " s";

  if (duration > 0) {
    posStr += " / " + QString::number(duration / GST_SECOND) + "." +
        QString::number((duration % GST_SECOND) / (GST_SECOND / 100), 10, 2).rightJustified(2, '0') + " s";
  }

  m_statusLabel->setText(posStr);
}

void MainWindow::createActions() {
  // File menu actions
  QAction* newAct = new QAction("&New", this);
  newAct->setShortcuts(QKeySequence::New);
  newAct->setStatusTip("Create a new pipeline");
  connect(newAct, &QAction::triggered, this, &MainWindow::newPipeline);

  QAction* openAct = new QAction("&Open", this);
  openAct->setShortcuts(QKeySequence::Open);
  openAct->setStatusTip("Open a pipeline file");
  connect(openAct, &QAction::triggered, this, &MainWindow::openPipeline);

  QAction* saveAct = new QAction("&Save", this);
  saveAct->setShortcuts(QKeySequence::Save);
  saveAct->setStatusTip("Save the pipeline");
  connect(saveAct, &QAction::triggered, this, &MainWindow::savePipeline);

  QAction* saveAsAct = new QAction("Save &As...", this);
  saveAsAct->setShortcuts(QKeySequence::SaveAs);
  saveAsAct->setStatusTip("Save the pipeline as a new file");
  connect(saveAsAct, &QAction::triggered, this, &MainWindow::savePipelineAs);

  QAction* exitAct = new QAction("E&xit", this);
  exitAct->setShortcuts(QKeySequence::Quit);
  exitAct->setStatusTip("Exit the application");
  connect(exitAct, &QAction::triggered, this, &QWidget::close);

  // Pipeline menu actions
  QAction* buildAct = new QAction("&Build", this);
  buildAct->setStatusTip("Build the pipeline");
  connect(buildAct, &QAction::triggered, this, &MainWindow::buildPipeline);

  QAction* runAct = new QAction("&Run", this);
  runAct->setStatusTip("Run the pipeline");
  connect(runAct, &QAction::triggered, this, &MainWindow::runPipeline);

  QAction* pauseAct = new QAction("&Pause", this);
  pauseAct->setStatusTip("Pause the pipeline");
  connect(pauseAct, &QAction::triggered, this, &MainWindow::pausePipeline);

  QAction* stopAct = new QAction("&Stop", this);
  stopAct->setStatusTip("Stop the pipeline");
  connect(stopAct, &QAction::triggered, this, &MainWindow::stopPipeline);

  QAction* visualizeAct = new QAction("&Visualize", this);
  visualizeAct->setStatusTip("Visualize the pipeline using dot");
  connect(visualizeAct, &QAction::triggered, this, &MainWindow::showPipelineDot);
}

void MainWindow::createMenus() {
  // File menu
  QMenu* fileMenu = menuBar()->addMenu("&File");
  fileMenu->addAction(findChild<QAction*>("&New"));
  fileMenu->addAction(findChild<QAction*>("&Open"));
  fileMenu->addSeparator();
  fileMenu->addAction(findChild<QAction*>("&Save"));
  fileMenu->addAction(findChild<QAction*>("Save &As..."));
  fileMenu->addSeparator();
  fileMenu->addAction(findChild<QAction*>("E&xit"));

  // Pipeline menu
  QMenu* pipelineMenu = menuBar()->addMenu("&Pipeline");
  pipelineMenu->addAction(findChild<QAction*>("&Build"));
  pipelineMenu->addAction(findChild<QAction*>("&Run"));
  pipelineMenu->addAction(findChild<QAction*>("&Pause"));
  pipelineMenu->addAction(findChild<QAction*>("&Stop"));
  pipelineMenu->addSeparator();
  pipelineMenu->addAction(findChild<QAction*>("&Visualize"));

  // View menu
  QMenu* viewMenu = menuBar()->addMenu("&View");
  viewMenu->addAction(m_elementDock->toggleViewAction());
  viewMenu->addAction(m_logDock->toggleViewAction());
  viewMenu->addAction(m_propertyDock->toggleViewAction());
}

void MainWindow::createToolbars() {
  // Pipeline toolbar
  QToolBar* pipelineToolbar = addToolBar("Pipeline");
  pipelineToolbar->addAction(findChild<QAction*>("&Build"));
  pipelineToolbar->addAction(findChild<QAction*>("&Run"));
  pipelineToolbar->addAction(findChild<QAction*>("&Pause"));
  pipelineToolbar->addAction(findChild<QAction*>("&Stop"));
}

void MainWindow::createDockWindows() {
  // Element library dock
  m_elementDock = new QDockWidget("Element Library", this);
  m_elementDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
  m_elementDock->setWidget(m_elementLibrary);
  addDockWidget(Qt::LeftDockWidgetArea, m_elementDock);

  // Log viewer dock
  m_logDock = new QDockWidget("Log Output", this);
  m_logDock->setAllowedAreas(Qt::BottomDockWidgetArea);
  m_logDock->setWidget(m_logViewer);
  addDockWidget(Qt::BottomDockWidgetArea, m_logDock);

  // Property editor dock
  m_propertyDock = new QDockWidget("Properties", this);
  m_propertyDock->setAllowedAreas(Qt::RightDockWidgetArea);
  m_propertyDock->setWidget(m_propertyEditor);
  addDockWidget(Qt::RightDockWidgetArea, m_propertyDock);
}

void MainWindow::createStatusBar() {
  m_statusLabel = new QLabel("Pipeline: Stopped");
  statusBar()->addWidget(m_statusLabel);
}

void MainWindow::setupConnections() {
  // Connect PipelineEditor signals to slots
  connect(m_pipelineEditor, &PipelineEditor::elementSelected, this, &MainWindow::showElementProperties);

  // Connect ElementLibrary signals to PipelineEditor slots
  connect(m_elementLibrary, &ElementLibrary::elementAdded, m_pipelineEditor, &PipelineEditor::addElement);

  // Connect PipelineModel signals to slots
  connect(m_pipelineModel, &GstPipelineModel::stateChanged, this, &MainWindow::onPipelineStateChanged);
  connect(m_pipelineModel, &GstPipelineModel::errorOccurred, this, &MainWindow::onPipelineError);
}

bool MainWindow::saveCurrentPipeline(const QString& filename) {
  if (filename.isEmpty())
    return false;

  QFile file(filename);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    m_logViewer->append("Failed to open file for writing: " + filename);
    return false;
  }

  QTextStream out(&file);
  out << m_pipelineModel->toString();
  file.close();

  setCurrentFile(filename);
  m_logViewer->append("Pipeline saved to: " + filename);
  return true;
}

void MainWindow::setCurrentFile(const QString& filename) {
  m_currentFilename = filename;
  m_isModified = false;
  updateWindowTitle();
}

void MainWindow::updateWindowTitle() {
  QString title = "GstPipelineStudio";

  if (!m_currentFilename.isEmpty()) {
    QFileInfo fileInfo(m_currentFilename);
    title += " - " + fileInfo.fileName();
  } else {
    title += " - [Untitled]";
  }

  if (m_isModified) {
    title += " *";
  }

  setWindowTitle(title);
}
