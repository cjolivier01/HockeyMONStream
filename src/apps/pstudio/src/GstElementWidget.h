#ifndef GSTELEMENTWIDGET_H
#define GSTELEMENTWIDGET_H

#include <QtCore/QObject>
#include <QtWidgets/QGraphicsItem>
#include <QtCore/QString>
#include <QtCore/QPointF>

// Use multiple inheritance to make GstElementWidget both a QObject and a QGraphicsItem
class GstElementWidget : public QObject, public QGraphicsItem
{
    Q_OBJECT
    
    // This properly tells Qt that this class implements the QGraphicsItem interface
    Q_PROPERTY(QGraphicsItem* graphicsItem READ graphicsItem)
    
public:
    explicit GstElementWidget(const QString &name);
    ~GstElementWidget();
    
    // Helper function for the Q_PROPERTY
    QGraphicsItem* graphicsItem() { return this; }
    
    // QGraphicsItem interface
    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget = nullptr) override;
    
    // Element properties
    QString name() const;
    
signals:
    // Proper Qt signals
    void moved(const QString &elementName, const QPointF &pos);
    void selected(const QString &elementName);
    void connectionRequested(const QString &srcElement, const QString &dstElement);
    void connectionRemoveRequested(const QString &srcElement, const QString &dstElement);
    
protected:
    // QGraphicsItem event handlers
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;
    void contextMenuEvent(QGraphicsSceneContextMenuEvent *event) override;
    
private:
    QString m_name;
    QRectF m_rect;
    bool m_isSelected;
    bool m_isMoving;
    QPointF m_initialPos;
};

#endif // GSTELEMENTWIDGET_H
