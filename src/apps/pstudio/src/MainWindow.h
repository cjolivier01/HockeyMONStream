#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "PipelineEditor.h"
#include "ElementLibrary.h"
#include "GstPipelineModel.h"

#include <QMainWindow>
#include <QDockWidget>
#include <QTextEdit>
#include <QLabel>
#include <QTimer>
#include <gst/gst.h>

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    bool loadPipelineFromString(const QString &pipelineString);
    bool loadPipelineFromFile(const QString &filename);

private slots:
    void newPipeline();
    void openPipeline();
    bool savePipeline();       // Changed from void to bool
    bool savePipelineAs();     // Changed from void to bool
    void runPipeline();
    void stopPipeline();
    void pausePipeline();
    void buildPipeline();
    void showPipelineDot();
    void showElementProperties(const QString &elementName);
    void onPipelineStateChanged(GstState state);
    void onPipelineError(const QString &message);
    void updatePipelineStatusMessage();

private:
    void createActions();
    void createMenus();
    void createToolbars();
    void createDockWindows();
    void createStatusBar();
    void setupConnections();
    bool saveCurrentPipeline(const QString &filename);
    void setCurrentFile(const QString &filename);
    void updateWindowTitle();
    
    // UI Components
    PipelineEditor *m_pipelineEditor;
    ElementLibrary *m_elementLibrary;
    QTextEdit *m_logViewer;
    QTextEdit *m_propertyEditor;
    QDockWidget *m_elementDock;
    QDockWidget *m_logDock;
    QDockWidget *m_propertyDock;
    QLabel *m_statusLabel;
    
    // Pipeline Management
    GstPipelineModel *m_pipelineModel;
    QString m_currentFilename;
    bool m_isModified;
    QTimer m_statusTimer;
};

#endif // MAINWINDOW_H
