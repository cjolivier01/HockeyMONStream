#pragma once

#include "hockeymom/csrc/play_tracker/BoxUtils.h"
#include "hstream/src/libs/common/ApplicationPayload.h"

namespace hm {
namespace playtracker {

#ifdef HAS_USER_APPLICATION_PAYLOAD

class PlayTrackerPayload : public UserApplicationPayload {
 public:
  PlayTrackerPayload(const hm::BBox& arena_box) : arena_box_(arena_box) {}
  static HmPayloadType PayloadSubType() {
    return HmPayloadType::HM_PAYLOAD_TYPE_PLAY_TRACKER;
  }

  UserApplicationPayload* CreateCopy() const override {
    return new PlayTrackerPayload(*this);
  }

  const hm::BBox& arena_box() const {
    return arena_box_;
  }

 private:
  hm::BBox arena_box_;
};

#endif // HAS_USER_APPLICATION_PAYLOAD

} // namespace playtracker
} // namespace hm
