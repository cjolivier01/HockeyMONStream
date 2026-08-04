#!/bin/bash
sudo apt-get install -y \
  curl \
  libva-dev \
  libsoup2.4-dev \
  libjson-glib-dev \
  libmosquitto-dev \
  libjsoncpp-dev \
  libglfw3-dev \
  protobuf-compiler \
  libgstrtspserver-1.0-dev \
  libgstreamer-plugins-bad1.0-dev \
  gstreamer1.0-rtsp \
  gstreamer1.0-nice \
  libglew-dev \
  libfftw3-dev \
  libv4l-dev \
  v4l-utils \
  v4l-conf \
  libgtk-3-dev \
  libtiff5-dev \
  qt6-base-dev \
  qt6-base-dev-tools \
  qt6-qpa-plugins \
  libyaml-cpp-dev \
  apt-file \
  libbluetooth-dev \
  aptitude

# DeepStream 9.1 uses TensorRT ABI 10. The unversioned NVIDIA metapackage now
# selects TensorRT 11, which makes HMStream's custom inference library
# incompatible with the DeepStream runtime. Select the newest CUDA 13.2 build
# that retains the TensorRT 10 ABI.
TENSORRT_VERSION="$(
  apt-cache madison tensorrt-dev |
    awk '$3 ~ /^10\./ && $3 ~ /[+]cuda13[.]2$/ { print $3; exit }'
)"
if [ -z "${TENSORRT_VERSION}" ]; then
  echo "Could not find a TensorRT 10 CUDA 13.2 development package." >&2
  exit 1
fi
sudo apt-get install -y "tensorrt-dev=${TENSORRT_VERSION}"
