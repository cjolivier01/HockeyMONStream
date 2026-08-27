# General Panini stitching samples

These samples were rendered from private copies of each game's current PTO and calibration images. The game
directories were not modified. “Before” reruns `nona` and `enblend` from the current optimized PTO; “after” uses
Hugin General Panini (`f19`, parameters `100,0,0`) with automatic field of view, canvas, and valid-image crop before
rerunning both tools. This is an offline A/B comparison; production calibration selects its projection before one
selected-projection mapping phase and a single `enblend` pass. Bounds-safety may rerender that same projection at a
smaller scale, but it never creates an intermediate panorama or starts a second projection/remap stage.

## `gse-16a`

This sample was restarted and regenerated on August 27, 2026 from the latest modified stitch files. Its source
fingerprints are:

```text
e5d8ae0ae70e6e4d22f8ec418c4c18a53b499082c56e773dee54c42828c0361c  hm_project.pto
163fbe143d65d877a77cd357299b79f9ee43debb2f198093a998414cd55db8bd  autooptimiser_out.pto
898eb917c70ac5cbc649b326b17997a38d291c797edbacc7520ac4ca8934a40e  left.png
cb436cd4652951a040571d1598215f8140d0044cc63bf527cbfd8ce190ee55b3  right.png
```

![gse-16a before and after](images/general-panini/gse-16a-before-after.jpg)

- [Larger before image](images/general-panini/gse-16a-before.jpg)
- [Larger after image](images/general-panini/gse-16a-after.jpg)

## `sharks-12-1-r4`

Source optimized-PTO fingerprint:

```text
0496e19b6d212b33f3a0c0ca4ca48f9f2e2aa2e02baa5f26f3899e7a1058f18b  autooptimiser_out.pto
```

![sharks-12-1-r4 before and after](images/general-panini/sharks-12-1-r4-before-after.jpg)

- [Larger before image](images/general-panini/sharks-12-1-r4-before.jpg)
- [Larger after image](images/general-panini/sharks-12-1-r4-after.jpg)
