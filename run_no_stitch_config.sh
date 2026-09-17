#!/bin/bash
./perf && bazel-bin/src/apps/hstream-cli/hstream-cli -c configs/ds_hockey_app_config.yaml  --enable-sources=URI-MULTIPLE --enable-sinks=ENCODE_FILE $@

