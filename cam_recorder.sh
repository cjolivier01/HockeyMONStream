#!/bin/bash
bazel-bin/src/apps/hstream-cli/hstream-cli \
  -c configs/dual_save_video.yaml \
  --enable-sources=CSI \
  --enable-sinks=ENCODE_FILE \
  --options=pipeline.hmaudio.enable=0 \
  --game-id=mylive \
  $@
