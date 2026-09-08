# HockeyMONStream licensing

HockeyMONStream is a source-available repository with mixed file-level
licensing. Do not assume that the root `LICENSE.md` applies to every file.

- Files covered by the root `LICENSE.md` are available under the MIT license.
- Some components are marked `SPDX-License-Identifier: Apache-2.0` and retain
  that license.
- NVIDIA-derived source files are marked
  `SPDX-License-Identifier: LicenseRef-NvidiaProprietary` and retain the NVIDIA
  copyright and proprietary-use notice in those files. An express NVIDIA
  license may be required for their use, reproduction, modification,
  disclosure, or distribution.
- Third-party libraries, tools, model files, and runtime dependencies retain
  their own licenses. In particular, NVIDIA DeepStream is a separately
  obtained dependency and is not licensed by this repository's MIT file.
- Jetson release packages redistribute Hugin components under GNU General
  Public License terms; the corresponding Hugin source archive and build
  script ship with each release.
- Notices for optional native model assets are recorded under
  `third_party/native_model_licenses/`.

The most specific license or notice associated with a file or dependency
controls. This summary is informational and is not legal advice.
