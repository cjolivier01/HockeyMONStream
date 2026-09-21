#include "hstream/src/gst-plugins/gst-fieldmask/dsfieldmask_lib.h"

#include "absl/status/status.h"

#include <iostream>

int main() {
  DsFieldMaskInitParams params;
  params.detection_mask_file = "";

  DsFieldMaskCtx* ctx = DsFieldMaskCtxInit(&params);
  if (!ctx) {
    std::cerr << "DsFieldMaskCtxInit returned nullptr" << std::endl;
    return 1;
  }

  const absl::Status status =
      DsFieldMaskProcessFrame(/*surface=*/nullptr, /*frame_index=*/0, /*frame_meta=*/nullptr, ctx, /*draw=*/false);
  if (!status.ok()) {
    std::cerr << "Expected no-op OK status, got: " << status << std::endl;
    DsFieldMaskCtxDeinit(ctx);
    return 2;
  }

  DsFieldMaskCtxDeinit(ctx);
  params.require_existing_mask = true;
  ctx = DsFieldMaskCtxInit(&params);
  const auto required = DsFieldMaskProcessFrame(nullptr, 0, nullptr, ctx, false);
  DsFieldMaskCtxDeinit(ctx);
  if (!absl::IsFailedPrecondition(required)) {
    std::cerr << "Require-existing-mask must reject an empty mask path, got: " << required << '\n';
    return 3;
  }
  return 0;
}
