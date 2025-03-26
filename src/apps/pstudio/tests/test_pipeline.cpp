#include "test_pipeline.h"

#include <QtTest/QtTest>
#include <gst/gst.h>


void TestPipeline::initTestCase()
{
    // Initialize GStreamer
    gst_init(nullptr, nullptr);
}

void TestPipeline::cleanupTestCase()
{
    // Nothing to do
}

void TestPipeline::testCreatePipeline()
{
    // Create a simple pipeline
    GstElement *pipeline = gst_pipeline_new("test-pipeline");
    QVERIFY(pipeline != nullptr);
    
    // Check pipeline state
    GstState state;
    gst_element_get_state(pipeline, &state, nullptr, 0);
    QCOMPARE(state, GST_STATE_NULL);
    
    // Cleanup
    gst_object_unref(pipeline);
}

void TestPipeline::testAddElement()
{
    // Create a pipeline
    GstElement *pipeline = gst_pipeline_new("test-pipeline");
    
    // Create a videotestsrc element
    GstElement *source = gst_element_factory_make("videotestsrc", "source");
    QVERIFY(source != nullptr);
    
    // Add the element to the pipeline
    QVERIFY(gst_bin_add(GST_BIN(pipeline), source));
    
    // Check that the element is in the pipeline
    GstElement *element = gst_bin_get_by_name(GST_BIN(pipeline), "source");
    QVERIFY(element != nullptr);
    gst_object_unref(element);
    
    // Cleanup
    gst_object_unref(pipeline);
}

void TestPipeline::testLinkElements()
{
    // Create a pipeline
    GstElement *pipeline = gst_pipeline_new("test-pipeline");
    
    // Create elements
    GstElement *source = gst_element_factory_make("videotestsrc", "source");
    GstElement *sink = gst_element_factory_make("fakesink", "sink");
    
    QVERIFY(source != nullptr);
    QVERIFY(sink != nullptr);
    
    // Add elements to the pipeline
    QVERIFY(gst_bin_add(GST_BIN(pipeline), source));
    QVERIFY(gst_bin_add(GST_BIN(pipeline), sink));
    
    // Link elements
    QVERIFY(gst_element_link(source, sink));
    
    // Check that elements are linked
    GstPad *srcPad = gst_element_get_static_pad(source, "src");
    GstPad *peerPad = gst_pad_get_peer(srcPad);
    QVERIFY(peerPad != nullptr);
    
    GstElement *peerElement = gst_pad_get_parent_element(peerPad);
    QCOMPARE(GST_OBJECT_NAME(peerElement), "sink");
    
    // Cleanup
    gst_object_unref(srcPad);
    gst_object_unref(peerPad);
    gst_object_unref(peerElement);
    gst_object_unref(pipeline);
}

void TestPipeline::testSetElementProperty()
{
    // Create a videotestsrc element
    GstElement *source = gst_element_factory_make("videotestsrc", "source");
    QVERIFY(source != nullptr);
    
    // Set a property
    g_object_set(G_OBJECT(source), "pattern", 1, NULL);
    
    // Get the property
    gint pattern;
    g_object_get(G_OBJECT(source), "pattern", &pattern, NULL);
    QCOMPARE(pattern, 1);
    
    // Cleanup
    gst_object_unref(source);
}

void TestPipeline::testRunPipeline()
{
    // Create a test pipeline
    GstElement *pipeline = createTestPipeline();
    
    // Set pipeline to playing state
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    QVERIFY(ret != GST_STATE_CHANGE_FAILURE);
    
    // Wait for a bit
    QTest::qWait(500);
    
    // Check pipeline state
    GstState state;
    gst_element_get_state(pipeline, &state, nullptr, GST_CLOCK_TIME_NONE);
    QCOMPARE(state, GST_STATE_PLAYING);
    
    // Stop pipeline
    gst_element_set_state(pipeline, GST_STATE_NULL);
    
    // Cleanup
    gst_object_unref(pipeline);
}

GstElement* TestPipeline::createTestPipeline()
{
    // Create a pipeline
    GstElement *pipeline = gst_pipeline_new("test-pipeline");
    
    // Create elements
    GstElement *source = gst_element_factory_make("videotestsrc", "source");
    GstElement *converter = gst_element_factory_make("videoconvert", "converter");
    GstElement *sink = gst_element_factory_make("fakesink", "sink");
    
    if (!source || !converter || !sink) {
        qFatal("Failed to create elements");
        return nullptr;
    }
    
    // Configure elements
    g_object_set(G_OBJECT(source), "num-buffers", 50, NULL);
    g_object_set(G_OBJECT(sink), "sync", FALSE, NULL);
    
    // Add elements to the pipeline
    gst_bin_add_many(GST_BIN(pipeline), source, converter, sink, NULL);
    
    // Link elements
    if (!gst_element_link_many(source, converter, sink, NULL)) {
        qFatal("Failed to link elements");
        gst_object_unref(pipeline);
        return nullptr;
    }
    
    return pipeline;
}

// No include of test_pipeline.moc here - it will be included by the genrule
