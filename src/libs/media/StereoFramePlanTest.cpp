#include "hstream/src/libs/media/StereoFramePlan.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
} // namespace
int main() {
  try {
    for (const auto& rate : {std::pair<int, int>{25, 1}, {30, 1}, {60000, 1001}}) {
      for (const double start : {0., .001, .04, .2, 60.}) {
        for (const double left : {0., .1, .5, .75, 2.878536}) {
          for (const double right : {0., .3, .5, 1.9}) {
            const auto plan = hm::media::PlanStereoFrames(start, left, right, rate.first, rate.second);
            Check(plan.ok(), "Valid stereo frame plan rejected");
            const long double requested = static_cast<long double>(start) * rate.first / rate.second + left;
            Check(
                plan->left_frame >= requested - 1e-10L && plan->left_frame < requested + 1 + 1e-10L,
                "Left frame does not start at or immediately after the requested content time");
            const auto residual = static_cast<long double>(plan->left_frame) - plan->right_frame - (left - right);
            Check(std::abs(residual) <= .500000001L, "Stereo synchronization residual exceeds half a frame");
            Check(
                plan->left_time_ns ==
                    static_cast<uint64_t>(
                        static_cast<__int128>(plan->left_frame) * rate.second * 1000000000 / rate.first),
                "Seek does not land on the source frame grid");
          }
        }
      }
    }
    auto tie = hm::media::PlanStereoFrames(0, .5, 0, 30, 1);
    Check(tie.ok() && tie->left_frame == 1 && tie->right_frame == 0, "Positive half-frame tie changed");
    tie = hm::media::PlanStereoFrames(0, 0, .5, 30, 1);
    Check(tie.ok() && tie->left_frame == 0 && tie->right_frame == 1, "Negative half-frame tie changed");
    tie = hm::media::PlanStereoFrames(0, .6, .1, 30, 1);
    Check(tie.ok() && tie->left_frame == 1 && tie->right_frame == 0, "Decimal positive half-frame tie changed");
    tie = hm::media::PlanStereoFrames(0, .1, .6, 30, 1);
    Check(tie.ok() && tie->left_frame == 1 && tie->right_frame == 2, "Decimal negative half-frame tie changed");
    tie = hm::media::PlanStereoFrames(0, .599999999, .1, 30, 1);
    Check(tie.ok() && tie->left_frame == 1 && tie->right_frame == 1, "Meaningfully sub-half offset was rounded up");
    const auto boundary = hm::media::PlanStereoFrames(.2, 0, 0, 30, 1);
    Check(
        boundary.ok() && boundary->left_frame == 6 && boundary->right_frame == 6,
        "Decimal .2-second start skipped a 30fps frame");
    const auto tenth = hm::media::PlanStereoFrames(.1, 0, 0, 30, 1);
    Check(tenth.ok() && tenth->left_frame == 3, "Decimal .1-second start skipped a frame");
    const auto fortieth = hm::media::PlanStereoFrames(.04, 0, 0, 25, 1);
    Check(fortieth.ok() && fortieth->left_frame == 1, "Decimal .04-second start skipped a 25fps frame");
    const auto after = hm::media::PlanStereoFrames(.0400000001, 0, 0, 25, 1);
    Check(after.ok() && after->left_frame == 2, "A start beyond the boundary was incorrectly snapped back");
    for (double invalid : {-1., std::numeric_limits<double>::infinity(), std::nan(""), 1e300}) {
      Check(!hm::media::PlanStereoFrames(invalid, 0, 0, 30, 1).ok(), "Invalid start accepted");
      Check(!hm::media::PlanStereoFrames(0, invalid, 0, 30, 1).ok(), "Invalid left offset accepted");
      Check(!hm::media::PlanStereoFrames(0, 0, invalid, 30, 1).ok(), "Invalid right offset accepted");
    }
    Check(!hm::media::PlanStereoFrames(0, 0, 0, 0, 1).ok(), "Invalid fps accepted");
    Check(!hm::media::PlanStereoFrames(0, 0, 0, 1, 0).ok(), "Invalid fps denominator accepted");
    Check(!hm::media::PlanStereoFrames(1e15, 0, 0, 1, 1).ok(), "Overflowed nanosecond clock accepted");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
