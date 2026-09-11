Stitched archives keep the pipeline's stitched canvas at its configured size. The
archive branch's `nvvideoconvert` scales the image on the GPU just before batch
demux and encoding, only when required by the selected encoder. Output dimensions
are even, preserve the canvas aspect ratio to rounding precision, and never
increase the image size. Tracking, preview, and the `max-output-width` stitching
setting retain their existing behavior.

The limit is queried for the selected codec and GPU at startup:

- Discrete GPUs and ARM64/SBSA use `NvEncGetEncodeCaps` for minimum/maximum width
  and height and the maximum macroblocks per frame. The pinned NVENC 11.1 header
  is used only for the API declarations; `libcuda.so.1` and
  `libnvidia-encode.so.1` come from the installed NVIDIA driver.
- Jetson uses the encoder device's `VIDIOC_ENUM_FRAMESIZES` query for H.264 or
  H.265. It uses `libv4l2` so the query follows the same NVIDIA device wrapper as
  the GStreamer encoder.

For example, the RTX 5090 tested here reports maximum dimensions of 8192x8192
for HEVC and 4096x4096 for H.264. The Jetson Orin tested here reports 7680x7680
for HEVC and 4096x4096 for H.264. These are queried values, not hardcoded
defaults. The `nvv4l2h264enc` / `nvv4l2h265enc` pad templates report unbounded
dimensions and therefore cannot establish the hardware limit.

A failed capability query produces a startup error. A canvas with an aspect
ratio that cannot fit both the minimum and maximum dimensions without upscaling
produces a stream error. Software encoding retains its existing behavior.

References:

- [NVIDIA Video Codec SDK programming guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/11.1/nvenc-video-encoder-api-prog-guide/index.html)
- [NVENC capability definitions](https://github.com/FFmpeg/nv-codec-headers/blob/n11.1.5.3/include/ffnvcodec/nvEncodeAPI.h)
- [V4L2 frame-size enumeration](https://docs.kernel.org/userspace-api/media/v4l/vidioc-enum-framesizes.html)

Validation tools:

```
bazelisk run --config=opt //src/apps/apps-common:encoder_dimensions_test
bazelisk run --config=opt //src/apps/apps-common:encoder_dimensions_gpu_test
bazelisk run --config=opt //src/apps/apps-common:encoder_dimensions_gpu_test -- h264
bazelisk run --config=opt //src/apps/apps-common:encoder_dimensions_gpu_test -- main10
```

The manual GPU test encodes three 10000x3000 synthetic frames and verifies both
the encoder's fitted NVMM input and a sibling branch's unchanged NVMM canvas.
On Jetson, add `--config=jetson` to these commands. For headless SSH checks,
unset a forwarded `DISPLAY` so NVIDIA EGL uses the local device.
