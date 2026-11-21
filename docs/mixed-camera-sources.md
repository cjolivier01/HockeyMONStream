# Mixed camera sources

Calibration keeps the native dimensions of each camera. On NVIDIA, differently sized recordings convert to the stitcher’s packed GPU format before batching so a common conversion surface cannot resize either input. A shared lens prescription initializes each differently sized image independently, including the lens-center shift in pixels. Automatic refinement can adjust each image. Equal-size pairs retain their existing optimizer settings.

New calibration provenance records the ordered source dimensions. NVIDIA and AMD compare decoded frames with those dimensions before remapping. Earlier calibrations remain readable. The camera Optics tabs show known source dimensions and flag matching optics on differently sized recordings. Check the field of view when using different capture modes. Lower angular resolution on one side can produce a visible change in sharpness at the seam.
