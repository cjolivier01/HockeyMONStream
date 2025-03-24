#include "GstElementWidget.h"

#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSceneContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QGraphicsScene>
#include <QInputDialog>
#include <QApplication>

// Since GstElementWidget can't be a QObject (multiple inheritance restrictions),
// we need to use a different approach for signals. For a real implementation,
// you might want to use a proxy QObject or redesign the class hierarchy.

GstElementWidget::GstElementWidget(const QString &name)
    : m_name(name)
    , m_rect(0, 0, 120, 60)
    , m_isSelected(false)
    , m_isMoving(false)
{
    setFlag(QGraphicsItem::ItemIsMovable);
    setFlag(QGraphicsItem::ItemIsSelectable);
    setFlag(QGraphicsItem::ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
}

GstElementWidget::~GstElementWidget()
{
}

QRectF GstElementWidget::boundingRect() const
{
    return m_rect;
}

void GstElementWidget::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget)
{
    Q_UNUSED(widget)
    
    // Draw the element rectangle
    QPen pen(Qt::black, 2);
    QBrush brush(Qt::lightGray);
    
    if (option->state & QStyle::State_Selected) {
        pen.setColor(Qt::blue);
        brush.setColor(Qt::lightBlue);
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

QString GstElementWidget::name() const
{
    return m_name;
}

void GstElementWidget::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_isMoving = true;
        m_initialPos = pos();
        
        // Emit selected signal
        QVariant selectedData;
        selectedData.setValue(m_name);
        QByteArray signalData;
        QDataStream ds(&signalData, QIODevice::WriteOnly);
        ds << selectedData;
        QCoreApplication::sendEvent(scene(), new QEvent(QEvent::User));
        
        // This is a hack to simulate a signal. In a real implementation,
        // you would use the proper signal mechanism or redesign the class.
    }
    
    QGraphicsItem::mousePressEvent(event);
}

void GstElementWidget::mouseMoveEvent(QGraphicsSceneMouseEvent *event)
{
    QGraphicsItem::mouseMoveEvent(event);
}

void GstElementWidget::mouseReleaseEvent(QGraphicsSceneMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_isMoving) {
        m_isMoving = false;
        
        // Emit moved signal
        if (pos() != m_initialPos) {
            // This is a hack to simulate a signal. In a real implementation,
            // you would use the proper signal mechanism or redesign the class.
            QVariant movedData;
            QMap<QString, QVariant> dataMap;
            dataMap["name"] = m_name;
            dataMap["pos"] = pos();
            movedData.setValue(dataMap);
            QByteArray signalData;
            QDataStream ds(&signalData, QIODevice::WriteOnly);
            ds << movedData;
            QCoreApplication::sendEvent(scene(), new QEvent(QEvent::User));
        }
    }
    
    QGraphicsItem::mouseReleaseEvent(event);
}

void GstElementWidget::contextMenuEvent(QGraphicsSceneContextMenuEvent *event)
{
    QMenu menu;
    
    QAction *renameAction = menu.addAction("Rename");
    QAction *deleteAction = menu.addAction("Delete");
    
    menu.addSeparator();
    
    QMenu *connectMenu = menu.addMenu("Connect to");
    QMenu *disconnectMenu = menu.addMenu("Disconnect from");
    
    // Add all other elements to the connect/disconnect menus
    if (scene()) {
        QList<QGraphicsItem*> items = scene()->items();
        
        for (QGraphicsItem *item : items) {
            GstElementWidget *elementWidget = dynamic_cast<GstElementWidget*>(item);
            
            if (elementWidget && elementWidget != this) {
                QAction *connectAction = connectMenu->addAction(elementWidget->name());
                connectAction->setData(elementWidget->name());
                
                QAction *disconnectAction = disconnectMenu->addAction(elementWidget->name());
                disconnectAction->setData(elementWidget->name());
            }
        }
    }
    
    QAction *selectedAction = menu.exec(event->screenPos());
    
    if (selectedAction == renameAction) {
        bool ok;
        QString newName = QInputDialog::getText(nullptr, "Rename Element",
                                               "New name:", QLineEdit::Normal,
                                               m_name, &ok);
        
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
        
        // Emit connection request signal
        QVariant connectData;
        QMap<QString, QVariant> dataMap;
        dataMap["src"] = m_name;
        dataMap["dst"] = targetElement;
        connectData.setValue(dataMap);
        QByteArray signalData;
        QDataStream ds(&signalData, QIODevice::WriteOnly);
        ds << connectData;
        QCoreApplication::sendEvent(scene(), new QEvent(QEvent::User));
    } else if (selectedAction && selectedAction->parent() == disconnectMenu) {
        QString targetElement = selectedAction->data().toString();
        
        // Emit connection remove request signal
        QVariant disconnectData;
        QMap<QString, QVariant> dataMap;
        dataMap["src"] = m_name;
        dataMap["dst"] = targetElement;
        disconnectData.setValue(dataMap);
        QByteArray signalData;
        QDataStream ds(&signalData, QIODevice::WriteOnly);
        ds << disconnectData;
        QCoreApplication::sendEvent(scene(), new QEvent(QEvent::User));
    }
}
