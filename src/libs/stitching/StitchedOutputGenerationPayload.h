#pragma once

#include "hstream/src/libs/common/ApplicationPayload.h"

#include <string>
#include <utility>

namespace hm::stitching {

#ifdef HAS_NVDS_CUSTOMUSERMETA
class StitchedOutputGenerationPayload : public UserApplicationPayload {
 public:
  explicit StitchedOutputGenerationPayload(std::string generation) : generation_(std::move(generation)) {}

  static HmPayloadType PayloadSubType() {
    return HmPayloadType::HM_PAYLOAD_TYPE_STITCHED_OUTPUT_GENERATION;
  }

  UserApplicationPayload* CreateCopy() const override {
    return new StitchedOutputGenerationPayload(*this);
  }

  const std::string& generation() const {
    return generation_;
  }

 private:
  std::string generation_;
};
#endif

} // namespace hm::stitching
