#!/bin/bash
set -e
make -j $(nproc) -C external/deepstream

