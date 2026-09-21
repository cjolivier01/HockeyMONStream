#include "PlayerFrameScanMask.h"

#include <gst/gst.h>

#include <iostream>
#include <string>

namespace {
using hm::pipeline::ValidatePlayerFrameScanMask;

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

struct Frame {
  NvDsBatchMeta* batch{nvds_create_batch_meta(1)};
  NvDsFrameMeta* frame{nvds_acquire_frame_meta_from_pool(batch)};

  Frame() {
    nvds_add_frame_meta_to_batch(batch, frame);
    frame->source_frame_width = frame->pipeline_width = 64;
    frame->source_frame_height = frame->pipeline_height = 48;
    frame->bInferDone = TRUE;
  }
  ~Frame() {
    nvds_destroy_batch_meta(batch);
  }
  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;

  bool output(const std::string& generation = "output-generation", const std::string& authority = "owner") {
    return hm::stitching::add_stitched_output_generation_meta(frame, generation, authority, {}, "hugin-generation");
  }
  void mask(
      const cv::Mat& pixels = cv::Mat(48, 64, CV_8UC1, cv::Scalar(255)),
      const std::string& revision = "output-generation:owner") {
    hm::UserApplicationPayload::create_and_add<hm::fieldmask::FieldMaskPayload>(
        frame, cv::Point2f(32, 24), cv::Rect2i(0, 0, 64, 48), pixels, revision);
  }
};
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  bool ok = true;
  ok &= expect(!ValidatePlayerFrameScanMask(nullptr).ok(), "null frames are not valid empty ice");
  {
    Frame skipped;
    ok &= skipped.output();
    skipped.mask();
    skipped.frame->bInferDone = FALSE;
    ok &= expect(!ValidatePlayerFrameScanMask(skipped.frame).ok(), "skipped inference cannot masquerade as empty ice");
  }
  {
    Frame unfiltered;
    ok &= unfiltered.output();
    ok &= expect(
        !ValidatePlayerFrameScanMask(unfiltered.frame).ok(), "missing mask payload fails even on inferred frames");
  }
  {
    Frame empty_mask;
    ok &= empty_mask.output();
    empty_mask.mask(cv::Mat());
    ok &= expect(!ValidatePlayerFrameScanMask(empty_mask.frame).ok(), "empty mask payload fails");
  }
  for (int type : {CV_16UC1, CV_8UC3}) {
    Frame wrong_type;
    ok &= wrong_type.output();
    wrong_type.mask(cv::Mat(48, 64, type, cv::Scalar::all(255)));
    ok &= expect(!ValidatePlayerFrameScanMask(wrong_type.frame).ok(), "mask must be single-channel eight-bit data");
  }
  for (const auto& size : {cv::Size(63, 48), cv::Size(64, 47)}) {
    Frame wrong_size;
    ok &= wrong_size.output();
    wrong_size.mask(cv::Mat(size, CV_8UC1, cv::Scalar(255)));
    ok &= expect(!ValidatePlayerFrameScanMask(wrong_size.frame).ok(), "width or height mismatch fails closed");
  }
  for (const std::string& revision : {"old-generation:owner", "output-generation:old-owner", ""}) {
    Frame stale;
    ok &= stale.output();
    stale.mask(cv::Mat(48, 64, CV_8UC1, cv::Scalar(255)), revision);
    ok &= expect(!ValidatePlayerFrameScanMask(stale.frame).ok(), "generation, authorization and empty revisions fail");
  }
  {
    Frame unknown_geometry;
    unknown_geometry.mask();
    ok &= expect(
        !ValidatePlayerFrameScanMask(unknown_geometry.frame).ok(), "mask without stitched output generation fails");
  }
  {
    Frame empty_ice;
    ok &= empty_ice.output();
    cv::Mat rink(48, 64, CV_8UC1, cv::Scalar(0));
    rink(cv::Rect(8, 8, 48, 32)).setTo(255);
    empty_ice.mask(rink);
    ok &= expect(
        empty_ice.frame->obj_meta_list == nullptr && ValidatePlayerFrameScanMask(empty_ice.frame).ok(),
        "an inferred frame with a matching actual rink mask may have zero retained people");
  }
  {
    Frame no_live_owner;
    ok &= no_live_owner.output("output-generation", "");
    no_live_owner.mask(cv::Mat(48, 64, CV_8UC1, cv::Scalar(255)), "output-generation:");
    ok &= expect(
        ValidatePlayerFrameScanMask(no_live_owner.frame).ok(),
        "matching empty authorization remains a valid offline generation");
  }
  {
    Frame destination;
    {
      Frame source;
      ok &= source.output();
      source.mask();
      nvds_copy_frame_user_meta_list(source.frame->frame_user_meta_list, destination.frame);
    }
    ok &= expect(
        ValidatePlayerFrameScanMask(destination.frame).ok(),
        "copied mask/generation metadata remains valid after original batch destruction");
  }
  return ok ? 0 : 1;
}
