#!/bin/bash
rm -f .registry-synced
pkill gxf_server
/opt/nvidia/graph-composer/registry cache -c
rm -rf /var/tmp/gxf/default_repo/*
set -e
/opt/nvidia/graph-composer/registry repo sync -n ngc-public
/opt/nvidia/graph-composer/registry repo sync -n default
touch .registry-synced
