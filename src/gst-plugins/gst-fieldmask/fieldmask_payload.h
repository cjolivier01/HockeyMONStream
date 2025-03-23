#pragma once

#include "hstream/src/libs/common/ApplicationPayload.h"

#include <opencv2/opencv.hpp>

namespace hm {
namespace fieldmask {

class FieldMaskPayload : public UserApplicationPayload {
 public:
  FieldMaskPayload(cv::Point2f centroid, const cv::Rect2i& field_box) : centroid_(centroid), field_box_(field_box) {}

  static HmPayloadType PayloadSubType() {
    return HmPayloadType::HM_PAYLOAD_TYPE_FIELDMASK;
  }

  UserApplicationPayload* CreateCopy() const override {
    return new FieldMaskPayload(*this);
  }

  const cv::Rect2i& field_box() const {
    return field_box_;
  }

 private:
  cv::Point2f centroid_;
  cv::Rect2i field_box_;
};

} // namespace fieldmask
} // namespace hm
