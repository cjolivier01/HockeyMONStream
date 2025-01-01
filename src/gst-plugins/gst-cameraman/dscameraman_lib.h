/*
 * SPDX-FileCopyrightText: Copyright (c) 2017-2020 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#ifndef __DSCAMERAMAN_LIB__
#define __DSCAMERAMAN_LIB__

#define MAX_LABEL_SIZE 128
#ifdef __cplusplus
extern "C" {
#endif

typedef struct DsCameraManCtx DsCameraManCtx;

// Init parameters structure as input, required for instantiating dscameraman_lib
typedef struct
{
  // Width at which frame/object will be scaled
  int processingWidth;
  // height at which frame/object will be scaled
  int processingHeight;
  // Flag to indicate whether operating on crops of full frame
  int fullFrame;
} DsCameraManInitParams;

// Detected/Labelled object structure, stores bounding box info along with label
typedef struct
{
  float left;
  float top;
  float width;
  float height;
  char label[MAX_LABEL_SIZE];
} DsCameraManObject;

// Output data returned after processing
typedef struct
{
  int numObjects;
  DsCameraManObject object[4];
} DsCameraManOutput;

// Initialize library context
DsCameraManCtx * DsCameraManCtxInit (DsCameraManInitParams *init_params);

// Dequeue processed output
DsCameraManOutput *DsCameraManProcess (DsCameraManCtx *ctx, unsigned char *data);

// Deinitialize library context
void DsCameraManCtxDeinit (DsCameraManCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif
