#!/bin/bash

export TRT_VERSION=10.13.2.6-1+cuda12.9

sudo apt purge 'libnvinfer*' 'tensorrt*' 'libnvonnxparsers*'
sudo apt clean
sudo apt update
sudo apt install \
  tensorrt-dev=$TRT_VERSION \
  libnvinfer-dev=$TRT_VERSION \
  libnvinfer-lean-dev=$TRT_VERSION \
  libnvinfer-dispatch-dev=$TRT_VERSION \
  libnvinfer-plugin-dev=$TRT_VERSION \
  libnvinfer-vc-plugin-dev=$TRT_VERSION \
  libnvonnxparsers-dev=$TRT_VERSION \
  libnvinfer-win-builder-resource10=$TRT_VERSION \
  libnvinfer-headers-python-plugin-dev=$TRT_VERSION \
  libnvinfer-headers-dev=$TRT_VERSION \
  libnvinfer10=$TRT_VERSION \
  libnvinfer-dispatch10=$TRT_VERSION \
  libnvinfer-headers-plugin-dev=$TRT_VERSION \
  libnvinfer-plugin10=$TRT_VERSION \
  libnvinfer-vc-plugin10=$TRT_VERSION \
  libnvinfer-lean10=$TRT_VERSION \
  libnvonnxparsers10=$TRT_VERSION
