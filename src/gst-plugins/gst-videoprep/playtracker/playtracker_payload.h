#pragma once

#include "hstream/src/libs/common/ApplicationPayload.h"

namespace hm {
namespace playtracker {

class PlayTrackerPayload : public UserApplicationPayload {
 public:
  static HmPayloadType PayloadSubType() { 
    return HmPayloadType::HM_PAYLOAD_TYPE_PLAY_TRACKER;
  }

  UserApplicationPayload* CreateCopy() const override {
    return new PlayTrackerPayload(*this);
  }

 private:
};

} // namespace playtracker
} // namespace hm
