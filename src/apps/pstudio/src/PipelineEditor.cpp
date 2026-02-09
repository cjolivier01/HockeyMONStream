#include "PipelineEditor.h"

#include <QtGui/QAction>
#include <QtGui/QContextMenuEvent>
#include <QtWidgets/QGraphicsLineItem>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtGui/QPen>
#include <QtWidgets/QVBoxLayout>

PipelineEditor::PipelineEditor(QWidget* parent)
    : QWidget(parent),
      m_scene(new QGraphicsScene(this)),
      m_view(new QGraphicsView(m_scene, this)),
      m_pipelineModel(nullptr) {
  createLayout();
}

PipelineEditor::~PipelineEditor() {}

void PipelineEditor::setPipelineModel(GstPipelineModel* model) {
  // Disconnect from old model
  if (m_pipelineModel) {
    disconnect(m_pipelineModel, &GstPipelineModel::pipelineModified, this, &PipelineEditor::onPipelineModified);
  }

  m_pipelineModel = model;

  // Connect to new model
  if (m_pipelineModel) {
    connect(m_pipelineModel, &GstPipelineModel::pipelineModified, this, &PipelineEditor::onPipelineModified);
  }

  // Update the scene
  updateScene();
}

void PipelineEditor::addElement(const QString& factoryName) {
  if (!m_pipelineModel) {
    return;
  }

  // Add the element to the model
  if (!m_pipelineModel->addElement(factoryName)) {
    QMessageBox::warning(this, "Error", "Failed to add element: " + m_pipelineModel->lastError());
    return;
  }

  // Update the scene
  updateScene();
}

void PipelineEditor::onElementMoved(const QString& elementName, const QPointF& pos) {
  // Store the element position for later restoration
  // (This would be added to the model in a full implementation)
}

void PipelineEditor::onElementSelected(const QString& elementName) {
  emit elementSelected(elementName);
}

void PipelineEditor::onConnectionRequest(const QString& srcElement, const QString& dstElement) {
  if (!m_pipelineModel) {
    return;
  }

  // In a real implementation, we would determine appropriate pads
  // For simplicity, we'll just use the default "src" and "sink" pads
  if (!m_pipelineModel->connectElements(srcElement, "src", dstElement, "sink")) {
    QMessageBox::warning(this, "Error", "Failed to connect elements: " + m_pipelineModel->lastError());
    return;
  }

  // Update the scene
  updateScene();
}

void PipelineEditor::onConnectionRemoveRequest(const QString& srcElement, const QString& dstElement) {
  if (!m_pipelineModel) {
    return;
  }

  // In a real implementation, we would determine the specific pads
  // For simplicity, we'll just use the default "src" and "sink" pads
  if (!m_pipelineModel->disconnectElements(srcElement, "src", dstElement, "sink")) {
    QMessageBox::warning(this, "Error", "Failed to disconnect elements: " + m_pipelineModel->lastError());
    return;
  }

  // Update the scene
  updateScene();
}

void PipelineEditor::onPipelineModified() {
  // Update the scene when the pipeline model changes
  updateScene();
}

void PipelineEditor::createLayout() {
  // Setup the view
  m_view->setRenderHint(QPainter::Antialiasing);
  m_view->setRenderHint(QPainter::SmoothPixmapTransform);
  m_view->setDragMode(QGraphicsView::RubberBandDrag);
  m_view->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);

  // Set up the scene
  m_scene->setSceneRect(0, 0, 2000, 2000);

  // Create layout
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_view);
  layout->setContentsMargins(0, 0, 0, 0);
  setLayout(layout);
}

void PipelineEditor::setupConnections() {
  // Will be used to connect signals from newly created element widgets
}

void PipelineEditor::updateScene() {
  if (!m_pipelineModel) {
    return;
  }

  // Clear the scene
  m_scene->clear();
  m_elementWidgets.clear();
  m_connectionLines.clear();

  // Get all elements from the model
  QStringList elements = m_pipelineModel->elements();

  // Create widgets for each element
  for (int i = 0; i < elements.size(); ++i) {
    QString elementName = elements[i];

    // Create a widget for the element
    GstElementWidget* widget = new GstElementWidget(elementName);

    // Position the widget
    // In a real implementation, we would restore the saved position
    widget->setPos(50 + (i % 5) * 150, 50 + (i / 5) * 100);

    // Connect signals
    connect(widget, &GstElementWidget::moved, this, &PipelineEditor::onElementMoved);
    connect(widget, &GstElementWidget::selected, this, &PipelineEditor::onElementSelected);
    connect(widget, &GstElementWidget::connectionRequested, this, &PipelineEditor::onConnectionRequest);
    connect(widget, &GstElementWidget::connectionRemoveRequested, this, &PipelineEditor::onConnectionRemoveRequest);

    // Add the widget to the scene
    m_scene->addItem(widget);

    // Store the widget for later use
    m_elementWidgets[elementName] = widget;
  }

  // Create connections between elements
  QList<QPair<QString, QString>> connections = m_pipelineModel->connections();

  for (const auto& conn : connections) {
    QString srcElement = conn.first;
    QString dstElement = conn.second;

    if (!m_elementWidgets.contains(srcElement) || !m_elementWidgets.contains(dstElement)) {
      continue;
    }

    GstElementWidget* srcWidget = m_elementWidgets[srcElement];
    GstElementWidget* dstWidget = m_elementWidgets[dstElement];

    // Create a line for the connection
    QGraphicsLineItem* line = new QGraphicsLineItem(
        srcWidget->pos().x() + srcWidget->boundingRect().width(),
        srcWidget->pos().y() + srcWidget->boundingRect().height() / 2,
        dstWidget->pos().x(),
        dstWidget->pos().y() + dstWidget->boundingRect().height() / 2);

    // Style the line
    QPen pen(Qt::black, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    line->setPen(pen);

    // Add the line to the scene
    m_scene->addItem(line);

    // Store the line for later use
    m_connectionLines[qMakePair(srcElement, dstElement)] = line;
  }
}
