#pragma once

#include <nvdsmeta.h>

#include "hstream/src/libs/player_analytics/Types.h"

namespace hm::player_analytics {

NvDsMetaType FrameResultMetaType();
bool ValidateFrameResult(const FrameResult& result) noexcept;

enum class FrameMetaFailureInjection { kNone, kAllocation, kPoolExhausted };

// Owns an immutable copy. Copies between tee branches share the immutable
// payload, with independent noexcept handle lifetime; no engine/surface pointers.
// Invalid contents, a duplicate attachment, allocation or pool failure returns false.
bool AttachFrameResult(
    NvDsFrameMeta* frame,
    const FrameResult& result,
    FrameMetaFailureInjection injection = FrameMetaFailureInjection::kNone) noexcept;
const FrameResult* FindFrameResult(const NvDsFrameMeta* frame) noexcept;

// Exercises the same noexcept handle-copy boundary installed on NvDsUserMeta.
bool FrameResultCopySucceedsForTest(const NvDsFrameMeta* frame, FrameMetaFailureInjection injection) noexcept;

} // namespace hm::player_analytics
