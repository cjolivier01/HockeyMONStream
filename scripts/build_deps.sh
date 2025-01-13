#!/bin/bash
set -e
pushd external/deepstream/sources/libs
CUDA_VERSION="$(nvcc --version | grep "release" | sed 's/,/ /g' | awk '{print$5}')"
CUDA_VER="${CUDA_VERSION}" make -k -j $(nproc) $@
popd
pushd external/deepstream/sources/gst-plugins
CUDA_VER="${CUDA_VERSION}" ./build_all.sh
popd
