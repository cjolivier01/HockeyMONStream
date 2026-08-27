#include "hstream/src/libs/stitching/LiveOutputEpoch.h"

#include <iostream>
#include <string>

int main() {
  const hm::stitching::LiveOutputEpoch original{
      .post_stitch_rotate_degrees = 21.125,
      .authorization_id = "authorization:with-delimiters",
      .scoreboard_property_value = "1,2,3,4,5,6,7,8",
  };
  const std::string serialized = hm::stitching::serialize_live_output_epoch(original);
  const auto parsed = hm::stitching::parse_live_output_epoch(serialized);
  const auto empty_authorization = hm::stitching::parse_live_output_epoch("0:-4.5");
  if (!parsed.ok() || parsed->post_stitch_rotate_degrees != original.post_stitch_rotate_degrees ||
      parsed->authorization_id != original.authorization_id || !empty_authorization.ok() ||
      parsed->scoreboard_property_value != original.scoreboard_property_value ||
      empty_authorization->post_stitch_rotate_degrees != -4.5 || !empty_authorization->authorization_id.empty() ||
      !empty_authorization->scoreboard_property_value.empty() ||
      hm::stitching::parse_live_output_epoch("missing-prefix").ok() ||
      hm::stitching::parse_live_output_epoch("20:short1").ok() ||
      hm::stitching::parse_live_output_epoch("0:20:short1").ok() ||
      hm::stitching::parse_live_output_epoch("0:nan").ok() ||
      hm::stitching::parse_live_output_epoch("0:1 trailing").ok()) {
    std::cerr << "FAIL: stitched-output epoch serialization did not round-trip safely\n";
    return 1;
  }
  return 0;
}
