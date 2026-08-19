# OpenColorIO grading-tone reference

`ShadowToneCurve.h` adapts the master Shadows segment from the VIDEO-style OpenColorIO
`GradingToneTransform`. OpenColorIO describes this transform as fine tonal-range correction and
defines the VIDEO shadow range with a black pivot of `0.0`, a shadow boundary of `0.6`, and a
validated adjustment range of `0.2` to `1.8`.

References:

- [OpenColorIO GradingToneTransform documentation](https://opencolorio.readthedocs.io/en/latest/api/grading_transforms.html#gradingtonetransform)
- [OpenColorIO grading-tone implementation](https://github.com/AcademySoftwareFoundation/OpenColorIO/tree/main/src/OpenColorIO/ops/gradingtone)

The adapted curve remains under OpenColorIO's BSD-3-Clause license in [LICENSE](LICENSE).
