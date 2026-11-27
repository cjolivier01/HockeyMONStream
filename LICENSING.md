# HockeyMONStream licensing

The default license for original HockeyMONStream code and documentation is the
GNU General Public License as published by the Free Software Foundation,
either version 3 or (at your option) any later version
(`GPL-3.0-or-later`). [Full GPLv3 text](LICENSE.md).

Chris Olivier also grants this license for the contributions he owns in
earlier HockeyMONStream revisions, back to their first appearance, wherever
the project default applied. Recipients of earlier copies may continue to
use those copies under the terms they received.

## Component licenses

- The imported DeepStream-Yolo code, configuration and documentation retain
  their [original MIT notice](third_party/DeepStream-Yolo-LICENSE.txt), including
  the copyright notices for Marcos Luciano Piropo Santos and NVIDIA.
- Files and components with their own terms keep those terms. Examples include
  the Apache-2.0 `dsxvideoconvert` plugin, the MIT-licensed `kmeans` library
  and the CC0 camera lens catalog.
- NVIDIA-derived files marked `LicenseRef-NvidiaProprietary` carry NVIDIA's
  proprietary-use notice. Use and redistribution of that material require the
  applicable NVIDIA permissions. NVIDIA DeepStream and other SDKs have their
  own license terms.
- Third-party tools and model assets carry their own terms. In the source
  tree, model notices are under `third_party/native_model_licenses/` and
  `configs/player-models/licenses/`. Current Debian and Jetson packages
  depend on the distribution's Hugin tools. Consult the notices accompanying
  older releases that bundled Hugin.
