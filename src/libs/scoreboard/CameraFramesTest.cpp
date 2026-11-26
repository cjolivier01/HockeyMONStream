#include "hstream/src/libs/scoreboard/CameraFrames.h"

#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
using Queue = hm::scoreboard::CameraFrames;
using Published = Queue::PublishResult;
using Selected = Queue::SelectionStatus;
using namespace std::chrono_literals;
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
Queue::Frame Frame(uint64_t epoch, int64_t time, int64_t end) {
  return {epoch, time, end, std::make_shared<const int64_t>(time)};
}
void Expect(Queue& queue, uint64_t epoch, int64_t time, Selected status, int64_t source = -1) {
  const auto selected = queue.TrySelect(epoch, time);
  Check(selected.status == status, "unexpected scoreboard selection status");
  if (status == Selected::kReady) {
    Check(
        selected.frame && selected.frame->epoch == epoch && selected.frame->time_ns == source,
        "selected wrong scoreboard frame");
    Check(*static_cast<const int64_t*>(selected.frame->image.get()) == source, "lost image ownership");
  } else {
    Check(!selected.frame, "failed selection exposed stale/future scoreboard pixels");
  }
}
void Publish(Queue& queue, const Queue::Frame& frame) {
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  for (;;) {
    const auto result = queue.TryPublish(frame);
    if (result == Published::kPublished)
      return;
    Check(result == Published::kFull && std::chrono::steady_clock::now() < deadline, "producer did not publish");
    std::this_thread::yield();
  }
}
void BlockingJoin() {
  Queue queue(2);
  Check(queue.Reset(1), "join initialization failed");
  auto joined = std::async(std::launch::async, [&] { return queue.WaitSelect(1, 100, 1s); });
  Check(joined.wait_for(5ms) == std::future_status::timeout, "compositor did not wait for missing camera frame");
  Publish(queue, Frame(1, 90, 110));
  const auto ready = joined.get();
  Check(ready.status == Selected::kReady && ready.frame->time_ns == 90, "join returned wrong presentation interval");
  Check(queue.WaitSelect(1, 110, 1ms).status == Selected::kTimeout, "join reused expired scoreboard frame");
  auto cancelled = std::async(std::launch::async, [&] { return queue.WaitSelect(1, 110, 1s); });
  queue.Cancel();
  Check(cancelled.get().status == Selected::kCancelled, "Stop did not wake waiting compositor");
  Check(queue.TryPublish(Frame(1, 110, 120)) == Published::kCancelled, "cancelled producer continued");
  Check(queue.Reset(2), "reset after cancellation failed");
  auto reset = std::async(std::launch::async, [&] { return queue.WaitSelect(2, 0, 1s); });
  Check(queue.Reset(3), "new seek epoch failed");
  Check(reset.get().status == Selected::kObsolete, "seek did not retire old compositor wait");
  auto ended = std::async(std::launch::async, [&] { return queue.WaitSelect(3, 0, 1s); });
  Check(queue.End(3), "EOS rejected");
  Check(ended.get().status == Selected::kEnded, "early scoreboard EOS did not wake compositor");
}
void ProducerBackpressure() {
  Queue queue(1);
  Check(queue.Reset(1), "producer epoch rejected");
  Publish(queue, Frame(1, 0, 10));
  auto producer = std::async(std::launch::async, [&] { return queue.WaitPublish(Frame(1, 10, 20), 1s); });
  Check(producer.wait_for(5ms) == std::future_status::timeout, "full queue overwrote a frame");
  Expect(queue, 1, 0, Selected::kReady, 0);
  Check(producer.get() == Published::kPublished, "consumer did not wake blocked producer");
  Check(queue.WaitPublish(Frame(1, 20, 30), 1ms) == Published::kTimeout, "producer timeout lost backpressure");
  auto stopped = std::async(std::launch::async, [&] { return queue.WaitPublish(Frame(1, 20, 30), 1s); });
  queue.Cancel();
  Check(stopped.get() == Published::kCancelled, "Stop did not wake blocked decoder");
  Check(queue.Reset(2), "producer restart rejected");
  Publish(queue, Frame(2, 0, 10));
  auto old = std::async(std::launch::async, [&] { return queue.WaitPublish(Frame(2, 10, 20), 1s); });
  Check(queue.Reset(3), "producer seek rejected");
  Check(old.get() == Published::kObsolete, "old producer crossed seek boundary");
}
} // namespace
int main() {
  try {
    Queue queue(2);
    Check(queue.TryPublish(Frame(0, 0, 10)) == Published::kObsolete, "published before initialization");
    Check(queue.Reset(1), "initial epoch rejected");
    Expect(queue, 1, 0, Selected::kPending);
    Publish(queue, Frame(1, 10, 20));
    Publish(queue, Frame(1, 20, 30));
    const auto pending = Frame(1, 30, 40);
    Check(queue.TryPublish(pending) == Published::kFull && pending.image, "producer backlog lost pending image");
    Expect(queue, 1, 9, Selected::kGap);
    Expect(queue, 1, 10, Selected::kReady, 10);
    auto held = queue.TrySelect(1, 10).frame;
    Publish(queue, pending);
    Expect(queue, 1, 19, Selected::kReady, 10);
    Expect(queue, 1, 20, Selected::kReady, 20);
    Expect(queue, 1, 29, Selected::kReady, 20);
    Expect(queue, 1, 30, Selected::kReady, 30);
    Expect(queue, 1, 39, Selected::kReady, 30);
    Expect(queue, 1, 40, Selected::kPending);
    Check(queue.WaitSelect(1, 40, 0ns).status == Selected::kTimeout, "zero-budget join silently skipped overlay");
    Check(queue.End(1), "EOS rejected");
    Expect(queue, 1, 40, Selected::kEnded);
    Check(queue.TryPublish(Frame(1, 40, 50)) == Published::kInvalid, "published after EOS");
    Check(!queue.Reset(1), "reused seek epoch admitted");
    Check(queue.Reset(2), "seek epoch rejected");
    Check(queue.TryPublish(Frame(1, 60, 70)) == Published::kObsolete, "old decoder frame crossed seek");
    Check(!queue.End(1), "old decoder EOS crossed seek");
    Expect(queue, 2, 0, Selected::kPending);
    Check(
        held && held->epoch == 1 && *static_cast<const int64_t*>(held->image.get()) == 10,
        "seek invalidated outstanding renderer ownership");
    Publish(queue, Frame(2, 0, 10));
    Check(queue.TryPublish(Frame(2, 0, 10)) == Published::kInvalid, "duplicate interval admitted");
    Check(queue.TryPublish(Frame(2, 5, 15)) == Published::kInvalid, "overlapping interval admitted");
    Check(queue.TryPublish(Frame(2, 10, 10)) == Published::kInvalid, "empty interval admitted");
    Check(queue.TryPublish(Frame(2, -1, 0)) == Published::kInvalid, "negative timestamp admitted");
    Check(queue.TryPublish({2, 10, 20, {}}) == Published::kInvalid, "empty GPU image admitted");
    Expect(queue, 1, 0, Selected::kObsolete);
    Expect(queue, 2, 9, Selected::kReady, 0);
    Expect(queue, 2, 8, Selected::kObsolete);
    Publish(queue, Frame(2, 20, std::numeric_limits<int64_t>::max()));
    Expect(queue, 2, 10, Selected::kGap);
    Expect(queue, 2, std::numeric_limits<int64_t>::max() - 1, Selected::kReady, 20);
    Expect(queue, 2, std::numeric_limits<int64_t>::max(), Selected::kPending);
    BlockingJoin();
    ProducerBackpressure();
    bool rejected = false;
    try {
      Queue invalid(0);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    Check(rejected, "unbounded/zero-capacity queue admitted");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
