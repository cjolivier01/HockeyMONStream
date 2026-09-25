# Player model catalog assets

These portable contracts bind each built-in model selection to its ONNX digest,
tensor names and shapes, preprocessing, and source provenance. Enabling a feature
and selecting its built-in model lets the native preparation path download the
ONNX and build or reuse a TensorRT engine for the local GPU and SDK. Normal
playback requires no Python export, manual bundle path, or environment variable.

| Feature | Model | Maximum batch | Distribution |
| --- | --- | ---: | --- |
| Pose | RTMPose-M, COCO 17 joints | 8 | HockeyMONStream release |
| Jersey | Hockey PARSeq, CC BY-NC 3.0; noncommercial | 8 | HockeyMONStream release |
| Action | STGCN++, COCO2D joint, NTU60 generic activities | 8 | HockeyMONStream release |
| ReID (default) | DeepStream supplied NvDCF ETLT, deployable_v1.0 | 32 | Installed SDK, or NVIDIA NGC |
| ReID (alternative) | NVIDIA ReIdentificationNet deployable_v1.2 ONNX | 32 | NVIDIA NGC directly |

`pose.json`, `jersey.json`, and `action.json` are the validated runtime bundle
manifests with only the hardware-specific `engine` section removed. Local native
preparation inserts that section after TensorRT engine creation. `model.onnx` is
the cache-local graph name; `provenance.json` records its digest and public URL.

`deepstream` reads the installed SDK's NvDCF accuracy profile and keeps its
`resnet50_market1501.etlt`/`keepAspc=1` preprocessing. The model digest is
`0e5b7f702ce7e3734e45f27b819866f15f6b083048481d1afae2089136e20201`
(NGC `deployable_v1.0`). Native preparation caps batch/history/workspace without
changing the model. SDK files are read-only; engine creation uses private cache staging.

`reid.json` describes the separately selected ONNX alternative
and has a separate native tracker contract. Its `tracker` map contains the
complete `ReID` overlay and `TrajectoryManagement` settings. Preparation replaces
`tracker.ReID.modelEngineFile` with the cached engine path and emits only the
tracker map for playback. The profile retains ReID type 2, batch 32, FP16
(`networkMode=1` in NvDCF), direct resize (`keepAspc=0`), and tracker-side feature
normalization. The NVIDIA ONNX itself is unchanged and is not mirrored here.

`notices/` gives attribution and conversion details. `licenses/` retains the
upstream license texts, including the hockey weights' CC BY-NC 3.0 terms and the
PARSeq implementation's separate Apache license and NOTICE. Automatic download
does not waive the hockey weights' noncommercial terms. NVIDIA's model-card
terms govern ReID; its preprocessing example's MIT notice applies only to the
example. The action labels are the original NTU60 vocabulary, not hockey event
labels. Conversion and runtime checks do not establish hockey accuracy.

The three mirrored graphs each have `.NOTICE.txt` and `.NOTICE.json` release
sidecars. The text sidecar includes complete license texts; the JSON sidecar
contains the portable contract and public provenance. `provenance.json` pins
their filenames, URLs, sizes where applicable, and SHA256 digests, as well as
source checkpoint/configuration and exporter identities. Published graphs are
self-contained ONNX files. Private image crops, skeleton fixtures, validation
tensors, raw export manifests, and TensorRT engines are excluded from release.

`src/libs/player_analytics/ModelCatalogData.inc` embeds these four JSON contracts
so selection and preparation do not depend on finding a source checkout. After
editing a contract, update its hash in `provenance.json` and regenerate the include
from the repository root:

```python
from pathlib import Path

root = Path("configs/player-models")
header = "// Generated from configs/player-models/*.json; regenerate as documented there.\n"
blocks = []
for feature in ("pose", "jersey", "action", "reid"):
    document = (root / f"{feature}.json").read_text()
    assert ')MODEL"' not in document
    blocks.append(
        f'constexpr const char k{feature.capitalize()}Contract[] = R"MODEL('
        + document + ')MODEL";\n'
    )
Path("src/libs/player_analytics/ModelCatalogData.inc").write_text(
    header + "\n" + "\n".join(blocks)
)
```
