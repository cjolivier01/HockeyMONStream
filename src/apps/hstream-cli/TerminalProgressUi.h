#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hm {

struct TerminalProgressOptions {
  int log_lines{11};
  int refresh_ms{1000};
  int start_threshold{0};
  bool show_graph{true};
  bool capture_output{true};
};

struct TerminalProgressStat {
  std::string label;
  std::string value;
};

struct TerminalProgressGraphNode {
  std::string name;
  int degree{0};
  bool active{false};
  std::optional<std::pair<int, int>> queue;
};

struct TerminalProgressGraphEdge {
  std::string from;
  std::string to;
};

struct TerminalProgressGraphSnapshot {
  std::vector<TerminalProgressGraphNode> nodes;
  std::vector<TerminalProgressGraphEdge> edges;
  std::vector<std::string> order;
  int max_degree{0};
  int concurrency_current{0};
  int concurrency_max{0};
  bool threaded{false};
};

struct TerminalProgressSnapshot {
  std::string title;
  std::vector<TerminalProgressStat> stats;
  uint64_t completed{0};
  std::optional<uint64_t> total;
  std::string completed_text;
  std::string total_text;
  bool complete{false};
};

struct TerminalProgressLineGlyphs {
  std::string horizontal;
  std::string vertical;
  std::string top_left;
  std::string top_right;
  std::string bottom_left;
  std::string bottom_right;
  std::string cross;
};

class TerminalProgressUi {
 public:
  explicit TerminalProgressUi(TerminalProgressOptions options);
  ~TerminalProgressUi();

  TerminalProgressUi(const TerminalProgressUi&) = delete;
  TerminalProgressUi& operator=(const TerminalProgressUi&) = delete;

  bool start();
  void stop();
  void restoreTerminalForInterrupt();
  bool started() const;

  void update(TerminalProgressSnapshot snapshot);
  void setGraphSnapshot(std::optional<TerminalProgressGraphSnapshot> graph);
  void appendLogLine(const std::string& line);

  static std::string renderForTest(
      const TerminalProgressOptions& options,
      const TerminalProgressSnapshot& snapshot,
      const std::optional<TerminalProgressGraphSnapshot>& graph,
      const std::vector<std::string>& logs,
      int width,
      int height);

 private:
  void captureLoop();
  void renderLoop();
  void requestRenderLocked();
  void renderOnce();
  std::string render(int width, int height) const;
  bool shouldRenderScreenLocked() const;
  std::pair<int, int> terminalSize() const;
  void writeToTerminal(const std::string& text) const;
  void restoreOutput();

  TerminalProgressOptions options_;
  mutable std::mutex mutex_;
  TerminalProgressSnapshot snapshot_;
  std::optional<TerminalProgressGraphSnapshot> graph_;
  TerminalProgressLineGlyphs glyphs_;
  std::deque<std::string> logs_;
  std::thread capture_thread_;
  std::thread render_thread_;
  int saved_stdout_fd_{-1};
  int saved_stderr_fd_{-1};
  int pipe_read_fd_{-1};
  bool capture_stdout_{false};
  bool started_{false};
  bool screen_started_{false};
  bool stop_requested_{false};
  bool dirty_{false};
  uint64_t update_count_{0};
  std::chrono::steady_clock::time_point last_render_time_{};
};

} // namespace hm
