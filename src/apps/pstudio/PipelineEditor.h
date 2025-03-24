#ifndef PIPELINEEDITOR_H
#define PIPELINEEDITOR_H

#include "GstElementWidget.h"
#include "GstPipelineModel.h"

#include <QWidget>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QMap>

class PipelineEditor : public QWidget
{
    Q_OBJECT

public:
    explicit PipelineEditor(QWidget *parent = nullptr);
    ~PipelineEditor();

    void setPipelineModel(GstPipelineModel *model);

public slots:
    void addElement(const QString &factoryName);
    
signals:
    void elementSelected(const QString &elementName);

private slots:
    void onElementMoved(const QString &elementName, const QPointF &pos);
    void onElementSelected(const QString &elementName);
    void onConnectionRequest(const QString &srcElement, const QString &dstElement);
    void onConnectionRemoveRequest(const QString &srcElement, const QString &dstElement);
    void onPipelineModified();

private:
    void createLayout();
    void setupConnections();
    void updateScene();
    
    QGraphicsScene *m_scene;
    QGraphicsView *m_view;
    GstPipelineModel *m_pipelineModel;
    QMap<QString, GstElementWidget*> m_elementWidgets;
    QMap<QPair<QString, QString>, QGraphicsLineItem*> m_connectionLines;
};

#endif // PIPELINEEDITOR_H
