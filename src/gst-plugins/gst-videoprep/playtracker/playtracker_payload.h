#pragma once

#include "hstream/src/libs/common/ApplicationPayload.h"

namespace hm {
namespace playtracker {

class PlayTrackerPayload : public UserApplicationPayload {
 public:
  PlayTrackerPayload(const hm::BBox arena_box) : arena_box_(arena_box) {}
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

} // namespace playtracker
} // namespace hm
