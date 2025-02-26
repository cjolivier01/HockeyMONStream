#!/bin/bash

gst-launch-1.0 nvarguscamerasrc sensor-id=$1 ! 'video/x-raw(memory:NVMM),width=3840,height=2160' ! nvvidconv flip-method=2  ! xvimagesink

