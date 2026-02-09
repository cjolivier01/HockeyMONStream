#!/bin/bash
# sudo systemctl restart nvargus-daemon
SENSOR_ID="${1:-0}"
echo Sendor id: ${SENSOR_ID}
gst-launch-1.0 nvarguscamerasrc sensor-id=${SENSOR_ID} ! \
  queue ! \
  'video/x-raw(memory:NVMM),width=1920,height=1080,framerate=60/1' ! \
  nvvidconv ! 'video/x-raw,width=1920,height=1080' ! \
  nv3dsink sync=false
