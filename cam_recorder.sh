#!/bin/bash
bazel-bin/src/apps/pipeline-app/pipeline-app \
  -c \
  configs/ds_hockey_configure_stitching.yaml \
  -c configs/dual_save_video.yaml \
  --enable-sources=CSI \
  --enable-sinks=ENCODE_FILE \
  --options=pipeline.hmaudio.enable=1 \
  --game-id=mylive \
  $@

