#!/bin/bash
sudo apt-get remove -y \
  curl \
  libva-dev \
  libsoup2.4-dev \
  libjson-glib-dev \
  libjsoncpp-dev \
  libglfw3-dev \
  protobuf-compiler \
  libgstrtspserver-1.0-dev \
  libgstreamer-plugins-bad1.0-dev \
  gstreamer1.0-rtsp \
  libglew-dev \
  libfftw3-dev \
  libv4l-dev \
  v4l-utils \
  v4l-conf \
  libgtk-3-dev \
  libtiff5-dev \
  libgtkglext1-dev \
  libyaml-cpp-dev \
  apt-file \
  aptitude

sudo apt-get remove -y tensorrt-dev
