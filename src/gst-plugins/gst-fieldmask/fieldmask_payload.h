#pragma once

#include "hstream/src/libs/common/ApplicationPayload.h"
#include "hstream/src/libs/common/IceBoundary.h"

#include <opencv2/core.hpp>
#include <memory>
#include <string>

namespace hm {
namespace fieldmask {

#ifdef HAS_NVDS_CUSTOMUSERMETA
class FieldMaskPayload : public UserApplicationPayload {
 public:
  FieldMaskPayload(
      cv::Point2f centroid,
      const cv::Rect2i& field_box,
      const cv::Mat& mask = {},
      std::string revision = {},
      IceBoundaryOffsets offsets = {},
      const cv::Mat& exclusion_mask = {})
      : revision_(std::move(revision)),
        centroid_(centroid),
        field_box_(field_box),
        mask_(mask),
        offsets_(offsets),
        exclusion_mask_(exclusion_mask.empty() ? mask : exclusion_mask) {}

  const IceBoundaryOffsets& offsets() const {
    return offsets_;
  }
  const cv::Mat& exclusion_mask() const {
    return exclusion_mask_;
  }

  const std::string& revision() const {
    return revision_;
  }

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
  std::string revision_;
  cv::Point2f centroid_;
  cv::Rect2i field_box_;
  cv::Mat mask_;
  IceBoundaryOffsets offsets_;
  cv::Mat exclusion_mask_;
};
#endif
} // namespace fieldmask
} // namespace hm
