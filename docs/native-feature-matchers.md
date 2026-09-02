# Native stitching feature matchers

`stitching.control_point_matcher` accepts four native, Python-free runtime
backends:

- `superpoint-lightglue` uses the existing SuperPoint + LightGlue ONNX graph.
- `dedode-lightglue` uses DeDoDe `L-C4-v2` detection, `B-upright`
  descriptors, and the `dedodeb` LightGlue weights in a fixed-shape ONNX
  graph. `scripts/export_dedode_lightglue_onnx.py` reproduces the release
  artifact from pinned checkpoint hashes.
- `loftr` uses the Apache-2.0 EfficientLoFTR outdoor optimized ONNX graph from
  `SpatialHub/efficient-loftr-onnx` revision
  `2c4515cbfd4866663db0ca1b3e02c55163dc5a75`. The UI spells out that this is
  the EfficientLoFTR variant rather than the original Kornia LoFTR graph.
- `akaze-hamming` uses OpenCV AKAZE with binary M-LDB descriptors, Hamming
  distance, a strict 0.75 Lowe ratio in both directions, and a mutual
  cross-check. It does not require a model asset.

The DeDoDe and LightGlue projects are MIT and Apache-2.0 respectively; Kornia
and the EfficientLoFTR artifact are Apache-2.0. Model files are downloaded at
runtime with pinned SHA-256 values and are not stored in Git. Matcher models
are on-demand assets: startup downloads only the graph selected by the layered
`stitching.control_point_matcher` configuration. Package builds verify and
stage every graph together with the notices in
`third_party/native_model_licenses`.

The sibling `video-stitcher` repository's CUDA AKAZE implementation has the
desired tolerance-level CPU parity, but that implementation is distributed as
part of an AGPL-3.0 project. It is not copied or linked into this MIT project.
The OpenCV implementation here independently reproduces its interoperable
AKAZE/M-LDB/Hamming behavior. It relies on HStream's downstream control-point
selection and robust homography fitting rather than copying video-stitcher's
overlap-ROI and black-border calibration heuristics. A CUDA implementation can
replace it after a compatibly licensed kernel is available.
