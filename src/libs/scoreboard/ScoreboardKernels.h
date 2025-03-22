#ifndef SCOREBOARD_KERNELS_H
#define SCOREBOARD_KERNELS_H

#include <cuda_runtime.h>

// #include "Scoreboard.h"

#ifdef __cplusplus
extern "C" {
#endif

// Launch a kernel to crop a rectangular ROI from an image.
// d_input: pointer to input image (inW x inH)
// d_output: pointer to output image (outW x outH)
// offX, offY: top‑left offset (in input) of the ROI.
void launchCropKernel(const uchar3* d_input, int inW, int inH,
                      uchar3* d_output, int outW, int outH,
                      int offX, int offY);

// Launch a kernel to bilinearly resize an image from (inW x inH) to (outW x outH).
void launchResizeKernel(const uchar3* d_input, int inW, int inH,
                        uchar3* d_output, int outW, int outH);

// Launch a warp–perspective kernel using a 3×3 transform provided as three float3 rows.
// The kernel maps output pixel (x,y) to source coordinate (u,v) using:
//     u = (m0.x*x + m0.y*y + m0.z) / (m2.x*x + m2.y*y + m2.z)
//     v = (m1.x*x + m1.y*y + m1.z) / (m2.x*x + m2.y*y + m2.z)
void launchWarpPerspectiveKernel(const uchar3* d_input, int inW, int inH,
                                 uchar3* d_output, int outW, int outH,
                                 float3 m0, float3 m1, float3 m2);

#ifdef __cplusplus
}
#endif

#endif // SCOREBOARD_KERNELS_H
