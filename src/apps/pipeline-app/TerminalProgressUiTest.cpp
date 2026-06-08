#include "TerminalProgressUi.h"

#include <unistd.h>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string strip_ansi(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    if (i + 1 < text.size() && text[i] == '\033' && text[i + 1] == '[') {
      i += 2;
      while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i++]);
        if (c >= 0x40 && c <= 0x7e) {
          break;
        }
      }
      continue;
    }
    out.push_back(text[i++]);
  }
  return out;
}

bool expect_contains(const std::string& text, const std::string& needle) {
  if (text.find(needle) != std::string::npos) {
    return true;
  }
  std::cerr << "Expected rendered progress UI to contain: " << needle << "\n";
  return false;
}

bool expect_not_contains(const std::string& text, const std::string& needle) {
  if (text.find(needle) == std::string::npos) {
    return true;
  }
  std::cerr << "Expected rendered progress UI not to contain: " << needle << "\n";
  return false;
}

bool expect_label_centered(const std::string& rendered, const std::string& label) {
  const size_t label_pos = rendered.find(label);
  if (label_pos == std::string::npos) {
    std::cerr << "Expected rendered progress UI to contain centered label: " << label << "\n";
    return false;
  }
  const std::string vertical = "│";
  const size_t left_bar = rendered.rfind(vertical, label_pos);
  const size_t right_bar = rendered.find(vertical, label_pos + label.size());
  if (left_bar == std::string::npos || right_bar == std::string::npos || right_bar <= left_bar + vertical.size()) {
    std::cerr << "Could not find panel bounds around centered label: " << label << "\n";
    return false;
  }
  const size_t content_start = left_bar + vertical.size();
  const size_t content_end = right_bar;
  const size_t left_spaces = label_pos > content_start ? label_pos - content_start : 0;
  const size_t label_end = label_pos + label.size();
  const size_t right_spaces = content_end > label_end ? content_end - label_end : 0;
  const size_t delta = left_spaces > right_spaces ? left_spaces - right_spaces : right_spaces - left_spaces;
  if (delta <= 1) {
    return true;
  }
  std::cerr << "Expected label to be centered: " << label << " left=" << left_spaces << " right=" << right_spaces
            << "\n";
  return false;
}

bool expect_level_symmetric(const std::string& rendered, const std::string& left_label, const std::string& right_label) {
  const size_t left_pos = rendered.find(left_label);
  const size_t right_pos = rendered.find(right_label);
  if (left_pos == std::string::npos || right_pos == std::string::npos) {
    std::cerr << "Expected rendered progress UI to contain symmetric labels: " << left_label << " / " << right_label
              << "\n";
    return false;
  }
  const std::string vertical = "│";
  const size_t left_bar = rendered.rfind(vertical, left_pos);
  const size_t right_bar = rendered.find(vertical, right_pos + right_label.size());
  if (left_bar == std::string::npos || right_bar == std::string::npos) {
    std::cerr << "Could not find panel bounds around symmetric labels\n";
    return false;
  }
  const size_t left_margin = left_pos - (left_bar + vertical.size());
  const size_t right_margin = right_bar - (right_pos + right_label.size());
  const size_t delta = left_margin > right_margin ? left_margin - right_margin : right_margin - left_margin;
  if (delta <= 1) {
    return true;
  }
  std::cerr << "Expected graph level to be symmetric: left=" << left_margin << " right=" << right_margin << "\n";
  return false;
}

size_t visible_width(const std::string& text) {
  size_t width = 0;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i++]);
    if (c >= 0xc0) {
      while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xc0) == 0x80) {
        ++i;
      }
    }
    ++width;
  }
  return width;
}

size_t max_line_width(const std::string& rendered) {
  size_t max_width = 0;
  size_t current_start = 0;
  for (size_t i = 0; i <= rendered.size(); ++i) {
    if (i == rendered.size() || rendered[i] == '\n') {
      max_width = std::max(max_width, visible_width(rendered.substr(current_start, i - current_start)));
      current_start = i + 1;
    }
  }
  return max_width;
}

size_t rendered_line_count(const std::string& rendered) {
  if (rendered.empty()) {
    return 0;
  }
  size_t lines = 0;
  for (char ch : rendered) {
    if (ch == '\n') {
      ++lines;
    }
  }
  return rendered.back() == '\n' ? lines : lines + 1;
}

bool expect_max_line_width(const std::string& rendered, size_t width) {
  const size_t actual = max_line_width(rendered);
  if (actual <= width) {
    return true;
  }
  std::cerr << "Expected rendered progress UI line width <= " << width << ", got " << actual << "\n";
  return false;
}

bool expect_line_count_at_most(const std::string& rendered, size_t lines) {
  const size_t actual = rendered_line_count(rendered);
  if (actual <= lines) {
    return true;
  }
  std::cerr << "Expected rendered progress UI line count <= " << lines << ", got " << actual << "\n";
  return false;
}

bool expect_non_tty_start_falls_back() {
  int pipe_fds[2] = {-1, -1};
  const int saved_stderr = dup(STDERR_FILENO);
  if (saved_stderr < 0 || pipe(pipe_fds) != 0) {
    std::cerr << "Could not set up non-TTY stderr test\n";
    if (saved_stderr >= 0) {
      close(saved_stderr);
    }
    return false;
  }
  if (dup2(pipe_fds[1], STDERR_FILENO) < 0) {
    std::cerr << "Could not redirect stderr for non-TTY test\n";
    close(saved_stderr);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return false;
  }
  close(pipe_fds[1]);

  hm::TerminalProgressOptions options;
  hm::TerminalProgressUi ui(options);
  const bool started = ui.start();

  dup2(saved_stderr, STDERR_FILENO);
  close(saved_stderr);
  close(pipe_fds[0]);
  if (!started) {
    return true;
  }
  ui.stop();
  std::cerr << "Expected progress UI not to start when stderr is not a TTY\n";
  return false;
}

} // namespace

int main() {
  hm::TerminalProgressOptions options;
  options.log_lines = 4;
  options.refresh_ms = 1000;
  options.show_graph = true;

  hm::TerminalProgressSnapshot snapshot;
  snapshot.title = "tv-12-1-r2";
  snapshot.stats = {
      {"Output FPS", "5.25 (5.10)"},
      {"Dataset length", "01:05:08"},
      {"Processed", "00:00:05"},
      {"Remaining", "01:05:03"},
      {"ETA", "00:12:24"},
      {"Speed", "4.20x"},
      {"Stitching", "ENABLED"},
      {"Audio", "2 bins"},
  };
  snapshot.completed = 5;
  snapshot.total = 10;
  snapshot.completed_text = "00:00:05";
  snapshot.total_text = "00:00:10";

  hm::TerminalProgressGraphSnapshot graph;
  graph.nodes = {
      {"source0", 0, true, std::nullopt},
      {"source1", 0, true, std::nullopt},
      {"streammux", 1, true, std::nullopt},
      {"hmstitcher", 2, true, std::nullopt},
      {"hmaudio0", 2, true, std::nullopt},
      {"sink3:UDPSINK", 3, true, std::nullopt},
  };
  graph.edges = {
      {"source0", "streammux"},
      {"source1", "streammux"},
      {"streammux", "hmstitcher"},
      {"hmstitcher", "sink3:UDPSINK"},
      {"hmaudio0", "sink3:UDPSINK"},
  };
  graph.order = {"source0", "source1", "streammux", "hmstitcher", "hmaudio0", "sink3:UDPSINK"};
  graph.max_degree = 3;
  graph.concurrency_current = 6;
  graph.concurrency_max = 6;
  graph.threaded = true;

  const std::vector<std::string> logs = {
      "startup line",
      "Enabled bins:",
      "  hmstitcher",
      "  hmaudio0",
  };

  const std::string rendered =
      strip_ansi(hm::TerminalProgressUi::renderForTest(options, snapshot, graph, logs, 120, 40));

  bool ok = true;
  ok &= expect_contains(rendered, "tv-12-1-r2");
  ok &= expect_contains(rendered, "┌");
  ok &= expect_contains(rendered, "─");
  ok &= expect_contains(rendered, "│");
  ok &= expect_contains(rendered, "┘");
  ok &= expect_contains(rendered, "Output FPS");
  ok &= expect_contains(rendered, "Dataset length");
  ok &= expect_contains(rendered, "Progress");
  ok &= expect_contains(rendered, "00:00:05/00:00:10");
  ok &= expect_contains(rendered, "Pipeline");
  ok &= expect_contains(rendered, "[#] source0");
  ok &= expect_contains(rendered, "hmstitcher");
  ok &= expect_contains(rendered, "hmaudio0");
  ok &= expect_contains(rendered, "sink3:UDPSINK");
  ok &= expect_contains(rendered, "Enabled bins:");
  ok &= expect_not_contains(rendered, "+---");
  ok &= expect_label_centered(rendered, "[#] streammux");
  ok &= expect_level_symmetric(rendered, "[#] source0", "[#] source1");

  const std::string narrow =
      strip_ansi(hm::TerminalProgressUi::renderForTest(options, snapshot, graph, logs, 60, 40));
  ok &= expect_max_line_width(narrow, 59);
  ok &= expect_contains(narrow, "Pipeline");

  const std::string fifty_columns =
      strip_ansi(hm::TerminalProgressUi::renderForTest(options, snapshot, graph, logs, 50, 40));
  ok &= expect_max_line_width(fifty_columns, 49);
  ok &= expect_not_contains(fifty_columns, "Pipeline");

  const std::string short_terminal =
      strip_ansi(hm::TerminalProgressUi::renderForTest(options, snapshot, graph, logs, 120, 12));
  ok &= expect_max_line_width(short_terminal, 119);
  ok &= expect_line_count_at_most(short_terminal, 12);
  ok &= expect_not_contains(short_terminal, "Pipeline");
  ok &= expect_contains(short_terminal, "Output FPS");
  ok &= expect_not_contains(short_terminal, "00:00:05/00:00:10│");

  ok &= expect_non_tty_start_falls_back();
  return ok ? 0 : 1;
}
