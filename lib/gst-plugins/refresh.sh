#!/bin/bash
rm -f ~/.cache/gstreamer-1.0/registry.x86_64.bin
gst-inspect-1.0 | grep dsexample
