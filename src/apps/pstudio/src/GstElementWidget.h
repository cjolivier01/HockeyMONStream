#ifndef GSTELEMENTWIDGET_H
#define GSTELEMENTWIDGET_H

#include <QGraphicsItem>
#include <QString>
#include <QPointF>

class GstElementWidget : public QGraphicsItem
{
public:
    explicit GstElementWidget(const QString &name);
    ~GstElementWidget();
    
    // QGraphicsItem interface
    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget = nullptr) override;
    
    // Element properties
    QString name() const;
    
signals:
    // Signal declarations (cannot be directly implemented as this is not a QObject)
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
