#include "hstream/src/apps/apps-common/PreviewRouteOwner.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using Owner = hm::preview::RouteOwner;
using Status = Owner::Status;

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

struct RouteFixture {
  std::string name;
  std::vector<std::string>* trace;
  bool ingress{false};
  bool drain{false};
  bool failed{false};
  bool quiesce_ok{true};

  void record(const std::string& operation) {
    trace->push_back(name + "." + operation);
  }
  Owner::Route route() {
    return {
        [this](bool enabled, std::uint64_t) {
          record(enabled ? "ingress-open" : "ingress-close");
          ingress = enabled && !failed;
        },
        [this](bool enabled, std::uint64_t) {
          record(enabled ? "drain-open" : "drain-close");
          drain = enabled && !failed;
        },
        [this] { return ingress && drain; },
        [this](std::uint64_t) { record("prepare"); },
        [this](std::uint64_t) {
          record("rearm");
          return ingress && drain;
        },
        [this](std::uint64_t) {
          record("quiesce");
          return !ingress && !drain && quiesce_ok;
        },
    };
  }
};

bool selection_and_lifecycle() {
  Owner owner;
  std::vector<std::string> trace;
  RouteFixture program{"program", &trace};
  RouteFixture source{"source0", &trace};
  if (!expect(
          owner.initialize_selection("none", 1) && owner.register_route("program", program.route()) &&
              owner.register_route("source0", source.route()),
          "Headless routes must register without window IDs or a GPU renderer") ||
      !expect(
          !owner.register_route("program", source.route()) && !owner.register_route("none", source.route()) &&
              !owner.initialize_selection("program", 99),
          "Duplicate registration and selection reset must not replace live route ownership"))
    return false;
  auto changed = [&] { trace.push_back("selection:" + owner.selected_channel()); };
  if (!expect(
          owner.select_current("program", 2, changed).status == Status::kActivated && program.ingress &&
              program.drain && owner.selected_channel() == "program" &&
              trace ==
                  std::vector<std::string>{
                      "selection:program", "program.prepare", "program.drain-open", "program.ingress-open"},
          "Metadata must be enabled before the drain and ingress admit the new route"))
    return false;
  trace.clear();
  if (!expect(
          owner.select_current("program", 3, changed).status == Status::kActivated &&
              trace == std::vector<std::string>{"program.rearm"},
          "Same-route recovery must rearm without waiting behind the consumer"))
    return false;
  trace.clear();
  if (!expect(
          owner.select_current("source0", 4, changed).status == Status::kActivated && !program.ingress &&
              !program.drain && source.ingress && source.drain &&
              trace ==
                  std::vector<std::string>{
                      "program.ingress-close",
                      "program.drain-close",
                      "program.quiesce",
                      "selection:source0",
                      "source0.prepare",
                      "source0.drain-open",
                      "source0.ingress-open"},
          "Switching must close ingress, drain, and quiesce the old consumer before opening another"))
    return false;
  trace.clear();
  if (!expect(
          owner.select_current("none", 4, changed).status == Status::kStale && trace.empty() && source.ingress,
          "Stale operations must not change native gates or metadata"))
    return false;
  if (!expect(
          owner.select_current("none", 5, changed).status == Status::kDeactivated && !source.ingress && !source.drain &&
              owner.selected_channel().empty() && owner.explicitly_disabled(),
          "Render-off must close the selected route and retain deliberate disablement"))
    return false;
  owner.forget_routes();
  trace.clear();
  if (!expect(
          owner.operation_generation() == 5 && owner.explicitly_disabled() && owner.channels().empty() &&
              owner.select_current("program", 6, changed).status == Status::kUnconfigured && trace.empty(),
          "Retiring native objects must forget borrowed callbacks while preserving the saved selection"))
    return false;
  if (!expect(
          owner.register_route("program", program.route()) &&
              owner.select_current("program", 6, changed).status == Status::kActivated,
          "Recreated routes must accept the next operation without resetting generations"))
    return false;
  owner.forget_route("program");
  owner.reconcile_selection();
  return expect(
      owner.selected_channel().empty() && !owner.explicitly_disabled(),
      "A missing source after topology replacement must not silently select a different camera");
}

bool session_fencing_and_failures() {
  Owner owner;
  std::vector<std::string> trace;
  RouteFixture program{"program", &trace};
  RouteFixture source{"source0", &trace};
  owner.initialize_selection("none", 1);
  owner.register_route("program", program.route());
  owner.register_route("source0", source.route());
  owner.bind_session({"run-a", 0, 20});
  Owner::Request request{1, {"run-a", 0, 20}, "request-1", 2, "program"};
  auto changed = [&] { trace.push_back("selection:" + owner.selected_channel()); };
  for (const auto& wrong : std::vector<Owner::Session>{{"run-b", 0, 20}, {"run-a", 1, 20}, {"run-a", 0, 19}}) {
    request.session = wrong;
    if (!expect(
            owner.select(request, changed).status == Status::kWrongSession && trace.empty() &&
                owner.operation_generation() == 1,
            "Run, stage, and topology must be validated before a queued operation mutates state"))
      return false;
  }
  request.session = owner.session();
  request.version = 2;
  if (!expect(
          owner.select(request, changed).status == Status::kInvalid && trace.empty(),
          "Unknown protocol versions must not mutate state"))
    return false;
  request.version = 1;
  request.channel = "arbitrary-element-name";
  if (!expect(
          owner.select(request, changed).status == Status::kInvalid && trace.empty(),
          "Only logical preview channels may be selected by the typed contract"))
    return false;
  request.channel = "program";
  if (!expect(
          owner.select(request, changed).status == Status::kActivated && owner.operation_generation() == 2,
          "Preview operation generations must be independent of topology generation 20"))
    return false;
  program.quiesce_ok = false;
  if (!expect(
          owner.select_current("source0", 3, changed).status == Status::kFailed && !program.ingress && !program.drain &&
              !source.ingress && owner.selected_channel().empty(),
          "Failure to quiesce must leave all routes closed"))
    return false;
  source.failed = true;
  if (!expect(
          owner.select_current("source0", 4, changed).status == Status::kFailed && !source.ingress && !source.drain &&
              owner.selected_channel().empty(),
          "A failed consumer must not leave an ingress running or metadata enabled"))
    return false;
  program.quiesce_ok = true;
  if (!expect(
          owner.select_current("program", 5, changed).status == Status::kActivated &&
              owner.select_current("source9", 6, changed).status == Status::kUnavailable && !program.ingress &&
              !program.drain && owner.selected_channel().empty() && !owner.explicitly_disabled(),
          "An unavailable camera must be reported without substituting another native route"))
    return false;
  return true;
}

bool waits_for_admitted_frame() {
  Owner owner;
  std::atomic<bool> ingress{true};
  std::atomic<bool> consumer_running{false};
  std::atomic<bool> quiesced{false};
  std::atomic<bool> drain_closed{false};
  std::mutex flow;
  std::mutex coordination;
  std::condition_variable ready;
  bool release = false;
  owner.initialize_selection("program", 1);
  owner.register_route(
      "program",
      {
          [&](bool enabled, std::uint64_t) { ingress = enabled; },
          [&](bool enabled, std::uint64_t) {
            std::lock_guard<std::mutex> lock(flow);
            drain_closed = !enabled;
          },
          [&] { return ingress && !drain_closed; },
          [](std::uint64_t) {},
          [](std::uint64_t) { return true; },
          [&](std::uint64_t) {
            quiesced = !consumer_running && drain_closed;
            return quiesced.load();
          },
      });
  auto frame = std::async(std::launch::async, [&] {
    std::lock_guard<std::mutex> lock(flow);
    consumer_running = true;
    ready.notify_all();
    std::unique_lock<std::mutex> coordination_lock(coordination);
    ready.wait(coordination_lock, [&] { return release; });
    consumer_running = false;
  });
  {
    std::unique_lock<std::mutex> lock(coordination);
    ready.wait(lock, [&] { return consumer_running.load(); });
  }
  auto disable = std::async(std::launch::async, [&] { return owner.select_current("none", 2, {}); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (ingress && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool barrier_waited =
      !ingress && !quiesced && disable.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout;
  {
    std::lock_guard<std::mutex> lock(coordination);
    release = true;
  }
  ready.notify_all();
  frame.get();
  const auto result = disable.get();
  return expect(
      barrier_waited && result.status == Status::kDeactivated && quiesced && !ingress && drain_closed,
      "Disable must stop ingress immediately and wait for the already admitted frame before quiescing");
}
} // namespace

int main() {
  return selection_and_lifecycle() && session_fencing_and_failures() && waits_for_admitted_frame() ? 0 : 1;
}
