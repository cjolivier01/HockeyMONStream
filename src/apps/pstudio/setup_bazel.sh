#!/bin/bash

echo "Setting up Bazel build files for GstPipelineStudio..."

# Create directory structure if needed
mkdir -p third_party
mkdir -p resources/styles
mkdir -p resources/icons
mkdir -p tests

# Copy Bazel build files
cp WORKSPACE .
cp BUILD .
cp resources/BUILD resources/
cp resources/styles/BUILD resources/styles/
cp resources/icons/BUILD resources/icons/
cp third_party/BUILD third_party/
cp third_party/qt.BUILD third_party/
cp third_party/gstreamer.BUILD third_party/
cp tests/BUILD tests/

echo "Bazel build files have been set up."
echo ""
echo "You may need to adjust paths in the following files to match your system:"
echo "- WORKSPACE: Update paths for Qt and GStreamer"
echo "- third_party/qt.BUILD: Update include paths and library names if needed"
echo "- third_party/gstreamer.BUILD: Update include paths and library names if needed"
echo ""
echo "To build the project with Bazel, run:"
echo "  bazel build //:gst_pipeline_studio"
echo ""
echo "To run tests, run:"
echo "  bazel test //tests:test_pipeline"
