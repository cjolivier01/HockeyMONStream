#!/bin/bash
sudo apt purge 'libnvinfer*' 'tensorrt*' 'libnvonnxparsers*'
sudo apt clean
sudo apt update
sudo apt install \
  tensorrt-dev=10.13.2.6-1+cuda12.9 \
  libnvinfer-dev=10.13.2.6-1+cuda12.9 \
  libnvinfer-lean-dev=10.13.2.6-1+cuda12.9 \
  libnvinfer-dispatch-dev=10.13.2.6-1+cuda12.9 \
  libnvinfer-plugin-dev=10.13.2.6-1+cuda12.9 \
  libnvinfer-vc-plugin-dev=10.13.2.6-1+cuda12.9 \
  libnvonnxparsers-dev=10.13.2.6-1+cuda12.9 \
  libnvinfer-win-builder-resource10=10.13.2.6-1+cuda12.9 \
  libnvinfer-headers-python-plugin-dev=10.13.2.6-1+cuda12.9 \
  libnvinfer-headers-dev=10.13.2.6-1+cuda12.9 \
  libnvinfer10=10.13.2.6-1+cuda12.9 \
  libnvinfer-dispatch10=10.13.2.6-1+cuda12.9 \
  libnvinfer-headers-plugin-dev=10.13.2.6-1+cuda12.9 \
  libnvinfer-plugin10=10.13.2.6-1+cuda12.9 \
  libnvinfer-vc-plugin10=10.13.2.6-1+cuda12.9 \
  libnvinfer-lean10=10.13.2.6-1+cuda12.9 \
  libnvonnxparsers10=10.13.2.6-1+cuda12.9
