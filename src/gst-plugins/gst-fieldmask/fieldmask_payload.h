#pragma once

#include "hstream/src/libs/common/ApplicationPayload.h"

#include <opencv2/opencv.hpp>

namespace hm {
namespace fieldmask {

#ifdef HAS_NVDS_CUSTOMUSERMETA
class FieldMaskPayload : public UserApplicationPayload {
 public:
  FieldMaskPayload(cv::Point2f centroid, const cv::Rect2i& field_box, const cv::Mat& mask = {})
      : centroid_(centroid), field_box_(field_box), mask_(mask) {}

  static HmPayloadType PayloadSubType() {
    return HmPayloadType::HM_PAYLOAD_TYPE_FIELDMASK;
  }

  UserApplicationPayload* CreateCopy() const override {
    return new FieldMaskPayload(*this);
  }

  const cv::Rect2i& field_box() const {
    return field_box_;
  }

  const cv::Point2f& centroid() const {
    return centroid_;
  }

  // Shares the immutable, CPU-resident calibration mask already used for
  // filtering. This does not reference or read a video surface.
  const cv::Mat& mask() const {
    return mask_;
  }

 private:
  cv::Point2f centroid_;
  cv::Rect2i field_box_;
  cv::Mat mask_;
};
#endif
} // namespace fieldmask
} // namespace hm
