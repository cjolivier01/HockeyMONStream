#include "GstElementWidget.h"

#include <QAction>
#include <QApplication>
#include <QGraphicsScene>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QStyleOptionGraphicsItem>

GstElementWidget::GstElementWidget(const QString& name)
    : QObject(nullptr) // Initialize QObject part with no parent
      ,
      QGraphicsItem() // Initialize QGraphicsItem part
      ,
      m_name(name),
      m_rect(0, 0, 120, 60),
      m_isSelected(false),
      m_isMoving(false) {
  setFlag(QGraphicsItem::ItemIsMovable);
  setFlag(QGraphicsItem::ItemIsSelectable);
  setFlag(QGraphicsItem::ItemSendsGeometryChanges);
  setAcceptHoverEvents(true);
}

GstElementWidget::~GstElementWidget() {}

QRectF GstElementWidget::boundingRect() const {
  return m_rect;
}

void GstElementWidget::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
  Q_UNUSED(widget)

  // Draw the element rectangle
  QPen pen(Qt::black, 2);
  QBrush brush(Qt::lightGray);

  if (option->state & QStyle::State_Selected) {
    pen.setColor(Qt::blue);
    // Use QColor instead of Qt::lightBlue (which doesn't exist)
    brush.setColor(QColor(173, 216, 230)); // A light blue color
  }

  painter->setPen(pen);
  painter->setBrush(brush);
  painter->drawRoundedRect(m_rect, 10, 10);

  // Draw the element name
  painter->setPen(Qt::black);
  painter->setFont(QFont("Arial", 10));
  painter->drawText(m_rect, Qt::AlignCenter, m_name);

  // Draw input and output ports
  painter->setPen(Qt::black);
  painter->setBrush(Qt::black);

  // Input port on the left
  painter->drawEllipse(QPointF(0, m_rect.height() / 2), 5, 5);

  // Output port on the right
  painter->drawEllipse(QPointF(m_rect.width(), m_rect.height() / 2), 5, 5);
}

QString GstElementWidget::name() const {
  return m_name;
}

void GstElementWidget::mousePressEvent(QGraphicsSceneMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    m_isMoving = true;
    m_initialPos = pos();

    // Emit selected signal
    emit selected(m_name);
  }

  QGraphicsItem::mousePressEvent(event);
}

void GstElementWidget::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
  QGraphicsItem::mouseMoveEvent(event);
}

void GstElementWidget::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
  if (event->button() == Qt::LeftButton && m_isMoving) {
    m_isMoving = false;

    // Emit moved signal if position changed
    if (pos() != m_initialPos) {
      emit moved(m_name, pos());
    }
  }

  QGraphicsItem::mouseReleaseEvent(event);
}

void GstElementWidget::contextMenuEvent(QGraphicsSceneContextMenuEvent* event) {
  QMenu menu;

  QAction* renameAction = menu.addAction("Rename");
  QAction* deleteAction = menu.addAction("Delete");

  menu.addSeparator();

  QMenu* connectMenu = menu.addMenu("Connect to");
  QMenu* disconnectMenu = menu.addMenu("Disconnect from");

  // Add all other elements to the connect/disconnect menus
  if (scene()) {
    QList<QGraphicsItem*> items = scene()->items();

    for (QGraphicsItem* item : items) {
      GstElementWidget* elementWidget = dynamic_cast<GstElementWidget*>(item);

      if (elementWidget && elementWidget != this) {
        QAction* connectAction = connectMenu->addAction(elementWidget->name());
        connectAction->setData(elementWidget->name());

        QAction* disconnectAction = disconnectMenu->addAction(elementWidget->name());
        disconnectAction->setData(elementWidget->name());
      }
    }
  }

  QAction* selectedAction = menu.exec(event->screenPos());

  if (selectedAction == renameAction) {
    bool ok;
    QString newName = QInputDialog::getText(nullptr, "Rename Element", "New name:", QLineEdit::Normal, m_name, &ok);

    if (ok && !newName.isEmpty()) {
      // In a real implementation, we would update the element name in the model
      m_name = newName;
      update();
    }
  } else if (selectedAction == deleteAction) {
    // In a real implementation, we would remove the element from the model
    scene()->removeItem(this);
    delete this;
  } else if (selectedAction && selectedAction->parent() == connectMenu) {
    QString targetElement = selectedAction->data().toString();
    emit connectionRequested(m_name, targetElement);
  } else if (selectedAction && selectedAction->parent() == disconnectMenu) {
    QString targetElement = selectedAction->data().toString();
    emit connectionRemoveRequested(m_name, targetElement);
  }
}
