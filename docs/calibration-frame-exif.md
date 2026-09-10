# Calibration frame EXIF

Calibration input PNGs (`left.png`, `right.png`, and additional sampled pairs)
retain camera metadata when their source recording contains GoPro GPMF or
Insta360 metadata. This applies to both one-pass and configure-only calibration.
The selected calibration pair keeps its EXIF when published to the game directory.

Available fields include camera make/model/serial, firmware, lens identity,
focal length/aperture, exposure time, ISO, exposure compensation, white balance,
sharpness, GPS position/altitude/speed and capture time. Fields are written only
when the recording supplies a supported value. Lens focal length is not guessed
from a field-of-view setting; gyro readings and other vendor data without an EXIF
counterpart are not copied into unrelated tags.

Each decoded frame carries its physical chapter URI and original presentation
timestamp through conversion and muxing. Consequently chapter changes, initial
seeks and camera synchronization do not reset the timestamp used to select
metadata. GPMF packets are selected by their interval, and exposure/ISO arrays
and GPS fixes within each packet are sampled at the frame's time. Insta360
exposure timestamps use the first-frame clock origin and clock units from its
protobuf metadata. Insta360 GPS timestamps are aligned to the chapter's
QuickTime UTC creation time; each fix covers at most one second, ending earlier
if the next fix arrives. Missing or invalid UTC origins skip these GPS records.
Unknown clocks and telemetry gaps do not inherit stale values. Capture time
uses the same chapter UTC origin plus the frame's original presentation timestamp.

ExifTool 13.50 is downloaded with a pinned SHA-256 by Bazel. Debian packages
(including Jetson and the Windows WSL installer) carry that same runtime under
`share/exiftool` and depend on `perl`. This avoids depending on the older
ExifTool versions supplied by Ubuntu 22.04/24.04. No separate ExifTool install is
needed for a Bazel build; Perl must be available on PATH.

Metadata is read once per chapter per calibration operation, with a 90-second
subprocess timeout and a 64 MiB extracted-metadata limit. Only EXIF-related tags
and timing are retained. PNG writing changes metadata chunks without decoding
or recompressing the pixels, and adds no GPU readback. Live sources and ordinary
videos without recognized camera metadata are unchanged. Missing or malformed
metadata logs a warning and leaves the calibration PNG usable; cancellation
still cancels calibration.

Run the metadata and transport tests with:

```sh
bazelisk test --config=opt --cpu=k8 \
  //src/libs/stitching:calibration_frame_exif_test \
  //src/libs/common:decoded_frame_sequence_meta_test \
  //src/libs/common:decoded_frame_sequence_meta_validation_test \
  //src/libs/common:decoded_frame_sequence_meta_dso_test
```

For a real-recording smoke check, first create a disposable PNG, then run:

```sh
bazel-bin/src/libs/stitching/calibration_frame_exif_test \
  /path/to/source.MP4 /path/to/disposable.png 10.5
exiftool -config '' -G1 -s -n -EXIF:all /path/to/disposable.png
```

The last argument is the original timestamp in seconds within that physical
source file, not the synchronized pipeline timestamp.
