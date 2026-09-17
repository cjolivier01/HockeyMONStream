bazel-bin/src/apps/hstream-cli/hstream-cli -c configs/ds_hockey_configure_stitching.yaml -c configs/ds_hockey_app_config.yaml  --enable-sources=CSI --enable-sinks=ENCODE_FILE --options=pipeline.hmaudio.enable=0 $@
# bazel-bin/src/apps/hstream-cli/hstream-cli -c configs/ds_hockey_app_config.yaml --enable-sources=URI-MULTIPLE --enable-sinks=ENCODE_FILE --options=pipeline.hmaudio.enable=1 $@
