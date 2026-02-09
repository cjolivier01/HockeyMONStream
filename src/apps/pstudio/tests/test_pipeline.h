#pragma once

#include <QtCore/QtCore>
#include <gst/gst.h>

class TestPipeline : public QObject
{
    Q_OBJECT
    
private slots:
    void initTestCase();
    void cleanupTestCase();
    
    void testCreatePipeline();
    void testAddElement();
    void testLinkElements();
    void testSetElementProperty();
    void testRunPipeline();
    
private:
    GstElement *createTestPipeline();
};
