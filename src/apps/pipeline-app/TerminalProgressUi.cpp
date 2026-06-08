#include "TerminalProgressUi.h"

#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <numeric>
#include <sstream>

namespace hm {
namespace {

constexpr const char* kReset = "\033[0m";
constexpr const char* kDim = "\033[90m";
constexpr const char* kWhiteOnDarkBlue = "\033[37;48;5;18m";
constexpr const char* kYellowOnDarkBlue = "\033[93;48;5;18m";
constexpr const char* kWhiteOnGrey35 = "\033[37;48;5;240m";
constexpr const char* kWhiteOnGrey23 = "\033[37;48;5;235m";
constexpr const char* kWhiteOnGrey19 = "\033[37;48;5;234m";
constexpr const char* kBrightGreenOnGrey35 = "\033[92;48;5;240m";
constexpr const char* kDimOnGrey35 = "\033[90;48;5;240m";
constexpr const char* kActiveMarker = "\033[92;48;5;234m";
constexpr const char* kActiveLabel = "\033[1;37;48;5;234m";
constexpr const char* kInactiveLabel = "\033[2;37;48;5;234m";

struct TerminfoApi {
  void* handle{nullptr};
  int (*setupterm)(char* term, int filedes, int* errret){nullptr};
  char* (*tigetstr)(char* capname){nullptr};
};

std::string repeat(char ch, size_t count) {
  return std::string(count, ch);
}

std::string repeat_text(const std::string& text, size_t count) {
  std::string out;
  out.reserve(text.size() * count);
  for (size_t i = 0; i < count; ++i) {
    out += text;
  }
  return out;
}

std::optional<size_t> terminal_sequence_end(const std::string& text, size_t pos) {
  const unsigned char first = static_cast<unsigned char>(text[pos]);
  if (first == 0x0e || first == 0x0f) {
    return pos + 1;
  }
  if (first != '\033' || pos + 1 >= text.size()) {
    return std::nullopt;
  }
  const char second = text[pos + 1];
  if (second == '[') {
    size_t end = pos + 2;
    while (end < text.size()) {
      const unsigned char c = static_cast<unsigned char>(text[end]);
      if (c >= 0x40 && c <= 0x7e) {
        return end + 1;
      }
      ++end;
    }
    return text.size();
  }
  if (second == ']') {
    size_t end = pos + 2;
    while (end < text.size()) {
      if (text[end] == '\a') {
        return end + 1;
      }
      if (text[end] == '\033' && end + 1 < text.size() && text[end + 1] == '\\') {
        return end + 2;
      }
      ++end;
    }
    return text.size();
  }
  if (second == '(' || second == ')' || second == '*' || second == '+' || second == '-' || second == '.') {
    return std::min(pos + 3, text.size());
  }
  return std::min(pos + 2, text.size());
}

std::string strip_terminal_sequences(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    if (std::optional<size_t> end = terminal_sequence_end(text, i)) {
      i = *end;
      continue;
    }
    out.push_back(text[i++]);
  }
  return out;
}

std::string truncate_visible_plain(std::string text, size_t width) {
  text = strip_terminal_sequences(text);
  std::string out;
  out.reserve(std::min(text.size(), width));
  size_t length = 0;
  for (size_t i = 0; i < text.size() && length < width;) {
    const size_t start = i;
    const unsigned char c = static_cast<unsigned char>(text[i++]);
    if (c >= 0xc0) {
      while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xc0) == 0x80) {
        ++i;
      }
    }
    out.append(text, start, i - start);
    ++length;
  }
  return out;
}

size_t visible_length(const std::string& text) {
  size_t length = 0;
  for (size_t i = 0; i < text.size();) {
    if (std::optional<size_t> end = terminal_sequence_end(text, i)) {
      i = *end;
      continue;
    }
    ++length;
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c >= 0xc0) {
      ++i;
      while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xc0) == 0x80) {
        ++i;
      }
      continue;
    }
    ++i;
  }
  return length;
}

std::string sanitize_line(const std::string& line) {
  std::string out;
  out.reserve(line.size());
  for (char ch : line) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (ch == '\r') {
      continue;
    }
    if (ch == '\t') {
      out.append("  ");
      continue;
    }
    if (c < 0x20 && ch != '\033') {
      out.push_back(' ');
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

std::string fit_visible(std::string text, size_t width) {
  if (visible_length(text) > width) {
    text = truncate_visible_plain(std::move(text), width);
  }
  const size_t length = visible_length(text);
  if (length < width) {
    text.append(width - length, ' ');
  }
  return text;
}

std::string truncate_plain(std::string text, size_t width) {
  return truncate_visible_plain(std::move(text), width);
}

std::string align_left(const std::string& text, size_t width) {
  return fit_visible(truncate_plain(text, width), width);
}

std::string align_right(const std::string& text, size_t width) {
  std::string plain = truncate_plain(text, width);
  if (plain.size() >= width) {
    return plain;
  }
  return repeat(' ', width - plain.size()) + plain;
}

TerminalProgressLineGlyphs unicode_line_glyphs() {
  return {
      .horizontal = "─",
      .vertical = "│",
      .top_left = "┌",
      .top_right = "┐",
      .bottom_left = "└",
      .bottom_right = "┘",
      .cross = "┼",
  };
}

const TerminfoApi* terminfo_api() {
  static TerminfoApi api = [] {
    TerminfoApi loaded;
    for (const char* library : {"libtinfo.so.6", "libtinfo.so", "libncursesw.so.6", "libncursesw.so"}) {
      void* handle = dlopen(library, RTLD_LAZY | RTLD_LOCAL);
      if (!handle) {
        continue;
      }
      loaded.setupterm = reinterpret_cast<int (*)(char*, int, int*)>(dlsym(handle, "setupterm"));
      loaded.tigetstr = reinterpret_cast<char* (*)(char*)>(dlsym(handle, "tigetstr"));
      if (loaded.setupterm && loaded.tigetstr) {
        loaded.handle = handle;
        return loaded;
      }
      dlclose(handle);
      loaded = {};
    }
    return loaded;
  }();
  return api.handle ? &api : nullptr;
}

std::string terminfo_capability(const TerminfoApi& api, const char* name) {
  char* value = api.tigetstr(const_cast<char*>(name));
  if (value == nullptr || value == reinterpret_cast<char*>(-1)) {
    return "";
  }
  return value;
}

TerminalProgressLineGlyphs terminal_line_glyphs(int fd) {
  const TerminfoApi* api = terminfo_api();
  if (!api) {
    return unicode_line_glyphs();
  }
  int err = 0;
  if (api->setupterm(nullptr, fd, &err) != 0 || err != 1) {
    return unicode_line_glyphs();
  }

  const std::string smacs = terminfo_capability(*api, "smacs");
  const std::string rmacs = terminfo_capability(*api, "rmacs");
  const std::string acsc = terminfo_capability(*api, "acsc");
  if (smacs.empty() || rmacs.empty() || acsc.empty()) {
    return unicode_line_glyphs();
  }

  std::map<char, char> acs_map;
  for (size_t i = 0; i + 1 < acsc.size(); i += 2) {
    acs_map[acsc[i]] = acsc[i + 1];
  }
  auto glyph = [&](char acs_key, const std::string& fallback) {
    auto found = acs_map.find(acs_key);
    if (found == acs_map.end()) {
      return fallback;
    }
    return smacs + std::string(1, found->second) + rmacs;
  };

  return {
      .horizontal = glyph('q', "─"),
      .vertical = glyph('x', "│"),
      .top_left = glyph('l', "┌"),
      .top_right = glyph('k', "┐"),
      .bottom_left = glyph('m', "└"),
      .bottom_right = glyph('j', "┘"),
      .cross = glyph('n', "┼"),
  };
}

std::string panel_top(
    size_t width,
    const std::string& title,
    const char* title_style,
    const TerminalProgressLineGlyphs& glyphs) {
  if (width < 2) {
    return "";
  }
  const size_t inner = width - 2;
  if (title.empty() || title.size() + 2 > inner) {
    return std::string(kDim) + glyphs.top_left + repeat_text(glyphs.horizontal, inner) + glyphs.top_right + kReset;
  }
  const std::string decorated_title = " " + title + " ";
  const size_t left = (inner - decorated_title.size()) / 2;
  const size_t right = inner - decorated_title.size() - left;
  return std::string(kDim) + glyphs.top_left + repeat_text(glyphs.horizontal, left) + title_style + decorated_title +
      kDim + repeat_text(glyphs.horizontal, right) + glyphs.top_right + kReset;
}

std::vector<std::string> make_panel(
    const std::vector<std::string>& content,
    size_t width,
    const std::string& title,
    const char* style,
    const TerminalProgressLineGlyphs& glyphs) {
  width = std::max<size_t>(width, 8);
  const size_t inner = width - 2;
  std::vector<std::string> lines;
  lines.reserve(content.size() + 2);
  lines.push_back(panel_top(width, title, kYellowOnDarkBlue, glyphs));
  const size_t content_width = inner > 2 ? inner - 2 : inner;
  auto padded_row = [inner, content_width](const std::string& row) {
    if (inner <= 2) {
      return fit_visible(row, inner);
    }
    return std::string(" ") + fit_visible(row, content_width) + " ";
  };
  if (content.empty()) {
    lines.push_back(
        std::string(kDim) + glyphs.vertical + kReset + style + padded_row("") + kReset + kDim +
        glyphs.vertical + kReset);
  } else {
    for (const std::string& row : content) {
      lines.push_back(
          std::string(kDim) + glyphs.vertical + kReset + style + padded_row(row) + kReset + kDim +
          glyphs.vertical + kReset);
    }
  }
  lines.push_back(
      std::string(kDim) + glyphs.bottom_left + repeat_text(glyphs.horizontal, inner) + glyphs.bottom_right + kReset);
  return lines;
}

std::vector<std::string> build_status_table(const TerminalProgressSnapshot& snapshot, size_t width) {
  const size_t inner = std::max<size_t>(width, 24);
  if (snapshot.stats.empty()) {
    return {""};
  }

  const size_t half = (snapshot.stats.size() + 1) / 2;
  const size_t pair_gap = 3;
  const size_t half_width = (inner > pair_gap) ? (inner - pair_gap) / 2 : inner / 2;
  const size_t label_width = std::min<size_t>(18, std::max<size_t>(8, half_width / 2));
  const size_t value_width = half_width > label_width + 1 ? half_width - label_width - 1 : half_width;

  std::vector<std::string> rows;
  for (size_t row = 0; row < half; ++row) {
    const TerminalProgressStat& left = snapshot.stats[row];
    std::string line = align_left(left.label, label_width) + " " + align_right(left.value, value_width);
    const size_t right_index = row + half;
    if (right_index < snapshot.stats.size()) {
      const TerminalProgressStat& right = snapshot.stats[right_index];
      line += repeat(' ', pair_gap) + align_left(right.label, label_width) + " " + align_right(right.value, value_width);
    }
    rows.push_back(std::move(line));
  }
  return rows;
}

std::string build_progress_row(const TerminalProgressSnapshot& snapshot, size_t width) {
  const std::string label = " Progress ";
  const std::string right = snapshot.total ? snapshot.completed_text + "/" + snapshot.total_text : snapshot.completed_text;
  const size_t right_len = visible_length(right);
  size_t bar_width = 4;
  if (width > label.size() + right_len + 3) {
    bar_width = width - label.size() - right_len - 3;
  }

  double fraction = 0.0;
  if (snapshot.total && *snapshot.total > 0) {
    fraction = static_cast<double>(std::min(snapshot.completed, *snapshot.total)) / static_cast<double>(*snapshot.total);
  } else if (snapshot.complete) {
    fraction = 1.0;
  }
  fraction = std::clamp(fraction, 0.0, 1.0);
  const size_t filled = static_cast<size_t>(std::round(fraction * static_cast<double>(bar_width)));

  std::ostringstream row;
  row << label;
  row << kBrightGreenOnGrey35 << repeat_text("━", std::min(filled, bar_width)) << kWhiteOnGrey35;
  if (filled < bar_width) {
    row << kDimOnGrey35 << repeat_text("━", bar_width - filled) << kWhiteOnGrey35;
  }
  row << " ";
  const size_t used = visible_length(row.str()) + right_len;
  if (used < width) {
    row << repeat(' ', width - used);
  }
  row << right;
  return row.str();
}

std::string truncate_label(const std::string& label, size_t width) {
  if (visible_length(label) <= width) {
    return label;
  }
  if (width <= 3) {
    return truncate_plain(label, width);
  }
  return truncate_plain(label, width - 3) + "...";
}

std::vector<std::string> build_log_rows(const std::vector<std::string>& logs, int max_lines) {
  std::vector<std::string> rows;
  const int count = std::max(1, max_lines);
  const int start = static_cast<int>(logs.size()) > count ? static_cast<int>(logs.size()) - count : 0;
  for (int i = start; i < static_cast<int>(logs.size()); ++i) {
    rows.push_back(sanitize_line(logs[i]));
  }
  while (static_cast<int>(rows.size()) < count) {
    rows.insert(rows.begin(), "");
  }
  return rows;
}

std::vector<size_t> centered_label_positions(
    const std::vector<TerminalProgressGraphNode>& nodes,
    const std::map<std::string, std::string>& labels,
    size_t graph_width,
    size_t slot_width) {
  const size_t count = nodes.size();
  if (count <= 0) {
    return {};
  }
  std::vector<size_t> positions;
  positions.reserve(count);

  auto clamp_label_start = [graph_width](size_t center, size_t label_width) {
    if (label_width >= graph_width) {
      return static_cast<size_t>(0);
    }
    const size_t half_label = label_width / 2;
    size_t start = center > half_label ? center - half_label : 0;
    if (start + label_width > graph_width) {
      start = graph_width - label_width;
    }
    return start;
  };

  if (count == 1) {
    const std::string& label = labels.at(nodes.front().name);
    return {clamp_label_start(graph_width / 2, label.size())};
  }

  const size_t left_center = slot_width / 2;
  const size_t right_center = graph_width > slot_width / 2 + 1 ? graph_width - slot_width / 2 - 1 : graph_width / 2;
  size_t last_end = 0;
  for (size_t idx = 0; idx < count; ++idx) {
    const double t = static_cast<double>(idx) / static_cast<double>(count - 1);
    const auto center =
        static_cast<size_t>(std::llround(static_cast<double>(left_center) +
                                         t * static_cast<double>(right_center - left_center)));
    const std::string& label = labels.at(nodes[idx].name);
    size_t start = clamp_label_start(center, label.size());
    if (idx > 0 && start < last_end + 1) {
      start = std::min(graph_width > label.size() ? graph_width - label.size() : 0, last_end + 1);
    }
    positions.push_back(start);
    last_end = start + label.size();
  }
  return positions;
}

std::string graph_symbol_to_glyph(char symbol, const TerminalProgressLineGlyphs& glyphs) {
  switch (symbol) {
    case '-':
      return glyphs.horizontal;
    case '|':
      return glyphs.vertical;
    case '+':
      return glyphs.cross;
    default:
      return " ";
  }
}

std::vector<std::string> build_graph_rows(
    const TerminalProgressGraphSnapshot& graph,
    size_t width,
    const TerminalProgressLineGlyphs& glyphs) {
  std::vector<std::string> rows;
  if (graph.threaded) {
    rows.push_back(" Concurrent: " + std::to_string(graph.concurrency_current) + "/" +
                   std::to_string(graph.concurrency_max));
  } else if (graph.concurrency_max > 0) {
    rows.push_back(" Concurrent: serial");
  }
  if (graph.nodes.empty()) {
    rows.push_back(" No pipeline nodes");
    return rows;
  }

  std::map<std::string, size_t> order_index;
  for (size_t i = 0; i < graph.order.size(); ++i) {
    order_index[graph.order[i]] = i;
  }

  std::map<int, std::vector<TerminalProgressGraphNode>> levels;
  std::map<std::string, int> node_degree;
  for (const TerminalProgressGraphNode& node : graph.nodes) {
    levels[node.degree].push_back(node);
    node_degree[node.name] = node.degree;
  }
  for (auto& item : levels) {
    std::sort(item.second.begin(), item.second.end(), [&order_index](const auto& a, const auto& b) {
      return order_index[a.name] < order_index[b.name];
    });
  }

  int max_nodes = 1;
  for (const auto& item : levels) {
    max_nodes = std::max<int>(max_nodes, item.second.size());
  }

  std::map<std::string, std::string> full_labels;
  size_t max_label_len = 0;
  for (const TerminalProgressGraphNode& node : graph.nodes) {
    std::string label = node.active ? "[#] " : "[ ] ";
    label += node.name;
    if (node.queue) {
      label += " q:" + std::to_string(node.queue->first) + "/" + std::to_string(node.queue->second);
    }
    max_label_len = std::max(max_label_len, label.size());
    full_labels[node.name] = label;
  }

  const size_t gap = 4;
  size_t label_limit = max_label_len;
  if (max_nodes > 1 && width > gap * static_cast<size_t>(max_nodes - 1)) {
    const size_t compact_slot_width = (width - gap * static_cast<size_t>(max_nodes - 1)) / max_nodes;
    if (compact_slot_width > 6) {
      label_limit = std::min(max_label_len, compact_slot_width - 2);
    }
  } else if (max_nodes == 1 && width > 2) {
    label_limit = std::min(max_label_len, width - 2);
  }

  std::map<std::string, std::string> labels;
  max_label_len = 0;
  for (const auto& item : full_labels) {
    std::string label = truncate_label(item.second, label_limit);
    max_label_len = std::max(max_label_len, visible_length(label));
    labels[item.first] = std::move(label);
  }

  const size_t slot_width = max_label_len + 2;
  const size_t required_graph_width = slot_width * max_nodes + gap * (max_nodes - 1);
  const size_t graph_width = std::max<size_t>(1, std::min(width, required_graph_width));
  const size_t graph_indent = width > graph_width ? (width - graph_width) / 2 : 0;

  std::map<int, std::vector<std::pair<TerminalProgressGraphNode, size_t>>> layouts_by_degree;
  std::map<std::string, size_t> node_centers;
  for (int degree = 0; degree <= graph.max_degree; ++degree) {
    auto found = levels.find(degree);
    if (found == levels.end()) {
      continue;
    }
    std::vector<size_t> positions = centered_label_positions(found->second, labels, graph_width, slot_width);
    for (size_t idx = 0; idx < found->second.size(); ++idx) {
      const TerminalProgressGraphNode& node = found->second[idx];
      const std::string& label = labels[node.name];
      const size_t pos = positions[idx];
      node_centers[node.name] = pos + label.size() / 2;
      layouts_by_degree[degree].push_back({node, pos});
    }
  }

  std::map<int, std::vector<TerminalProgressGraphEdge>> edges_by_level;
  for (const TerminalProgressGraphEdge& edge : graph.edges) {
    auto src = node_degree.find(edge.from);
    auto dst = node_degree.find(edge.to);
    if (src != node_degree.end() && dst != node_degree.end() && dst->second == src->second + 1) {
      edges_by_level[src->second].push_back(edge);
    }
  }

  auto merge_char = [](char old_ch, char new_ch) {
    if (old_ch == ' ') {
      return new_ch;
    }
    if (old_ch == new_ch) {
      return old_ch;
    }
    if ((old_ch == '|' || old_ch == '-' || old_ch == '+') && (new_ch == '|' || new_ch == '-' || new_ch == '+')) {
      return '+';
    }
    return old_ch;
  };

  for (int degree = 0; degree <= graph.max_degree; ++degree) {
    auto layout_found = layouts_by_degree.find(degree);
    if (layout_found == layouts_by_degree.end()) {
      continue;
    }
    std::string row(graph_width, ' ');
    std::vector<std::pair<size_t, bool>> marker_starts;
    std::vector<std::pair<std::pair<size_t, size_t>, bool>> label_spans;
    for (const auto& layout : layout_found->second) {
      const TerminalProgressGraphNode& node = layout.first;
      const std::string& label = labels[node.name];
      const size_t pos = layout.second;
      for (size_t i = 0; i < label.size() && pos + i < row.size(); ++i) {
        row[pos + i] = label[i];
      }
      marker_starts.push_back({pos, node.active});
      label_spans.push_back({{pos, std::min(pos + label.size(), row.size())}, node.active});
    }

    std::string colored_row;
    size_t cursor = 0;
    for (const auto& span : label_spans) {
      const size_t start = span.first.first;
      const size_t end = span.first.second;
      if (start > cursor) {
        colored_row += row.substr(cursor, start - cursor);
      }
      const bool active = span.second;
      const bool marker = std::any_of(marker_starts.begin(), marker_starts.end(), [start, end](const auto& item) {
        return item.first == start && start + 3 <= end;
      });
      if (marker && end >= start + 3) {
        colored_row += active ? kActiveMarker : kInactiveLabel;
        colored_row += row.substr(start, 3);
        colored_row += active ? kActiveLabel : kInactiveLabel;
        colored_row += row.substr(start + 3, end - start - 3);
      } else {
        colored_row += active ? kActiveLabel : kInactiveLabel;
        colored_row += row.substr(start, end - start);
      }
      colored_row += kWhiteOnGrey19;
      cursor = end;
    }
    if (cursor < row.size()) {
      colored_row += row.substr(cursor);
    }
    rows.push_back(repeat(' ', graph_indent) + std::move(colored_row));

    auto edges_found = edges_by_level.find(degree);
    if (edges_found != edges_by_level.end()) {
      std::string connector_symbols(graph_width, ' ');
      for (const TerminalProgressGraphEdge& edge : edges_found->second) {
        auto src = node_centers.find(edge.from);
        auto dst = node_centers.find(edge.to);
        if (src == node_centers.end() || dst == node_centers.end()) {
          continue;
        }
        const size_t x1 = src->second;
        const size_t x2 = dst->second;
        if (x1 == x2) {
          connector_symbols[x1] = merge_char(connector_symbols[x1], '|');
          continue;
        }
        const size_t start = std::min(x1, x2);
        const size_t end = std::max(x1, x2);
        connector_symbols[start] = merge_char(connector_symbols[start], '+');
        connector_symbols[end] = merge_char(connector_symbols[end], '+');
        for (size_t i = start + 1; i < end; ++i) {
          connector_symbols[i] = merge_char(connector_symbols[i], '-');
        }
      }
      std::string connector;
      for (char symbol : connector_symbols) {
        connector += graph_symbol_to_glyph(symbol, glyphs);
      }
      rows.push_back(repeat(' ', graph_indent) + std::string(kDim) + connector + kWhiteOnGrey19);
    }
  }
  return rows;
}

std::string render_compact_screen(
    const TerminalProgressSnapshot& snapshot,
    const std::vector<std::string>& logs,
    const TerminalProgressLineGlyphs& glyphs,
    size_t width,
    int terminal_height) {
  const size_t outer_width = width > 1 ? width - 1 : width;
  const size_t inner_width = outer_width > 2 ? outer_width - 2 : outer_width;
  const int max_lines = std::max(1, terminal_height);
  std::vector<std::string> lines;
  auto padded_compact_row = [inner_width](const std::string& row) {
    if (inner_width <= 2) {
      return fit_visible(row, inner_width);
    }
    return std::string(" ") + fit_visible(row, inner_width - 2) + " ";
  };

  const std::string title = snapshot.title.empty() ? "hstream" : snapshot.title;
  if (max_lines <= 2 || outer_width < 12) {
    lines.push_back(fit_visible(title, outer_width));
  } else {
    lines.push_back(std::string(kDim) + glyphs.top_left + repeat_text(glyphs.horizontal, inner_width) + glyphs.top_right +
                    kReset);
    lines.push_back(std::string(kDim) + glyphs.vertical + kReset + kWhiteOnDarkBlue + padded_compact_row(title) +
                    kReset + kDim + glyphs.vertical + kReset);
    if (!snapshot.stats.empty() && static_cast<int>(lines.size()) + 2 < max_lines) {
      lines.push_back(std::string(kDim) + glyphs.vertical + kReset + kWhiteOnGrey35 +
                      padded_compact_row(snapshot.stats.front().label + " " + snapshot.stats.front().value) +
                      kReset + kDim + glyphs.vertical + kReset);
    }
    if (static_cast<int>(lines.size()) + 2 < max_lines) {
      lines.push_back(std::string(kDim) + glyphs.vertical + kReset + kWhiteOnGrey35 +
                      padded_compact_row(build_progress_row(snapshot, inner_width > 2 ? inner_width - 2 : inner_width)) +
                      kReset + kDim + glyphs.vertical + kReset);
    }
    if (!logs.empty() && static_cast<int>(lines.size()) + 2 < max_lines) {
      lines.push_back(std::string(kDim) + glyphs.vertical + kReset + kWhiteOnGrey23 +
                      padded_compact_row(sanitize_line(logs.back())) + kReset + kDim + glyphs.vertical + kReset);
    }
    lines.push_back(std::string(kDim) + glyphs.bottom_left + repeat_text(glyphs.horizontal, inner_width) +
                    glyphs.bottom_right + kReset);
  }

  if (static_cast<int>(lines.size()) > max_lines) {
    lines.resize(max_lines);
  }
  std::ostringstream out;
  for (const std::string& line : lines) {
    out << fit_visible(line, outer_width) << "\n";
  }
  out << "\033[J";
  return out.str();
}

std::string render_screen(
    const TerminalProgressOptions& options,
    const TerminalProgressSnapshot& snapshot,
    const std::optional<TerminalProgressGraphSnapshot>& graph,
    const std::vector<std::string>& logs,
    const TerminalProgressLineGlyphs& glyphs,
  int terminal_width,
  int terminal_height) {
  const size_t width = static_cast<size_t>(std::max(terminal_width, 1));
  if (terminal_width <= 50 || terminal_height < 18) {
    return render_compact_screen(snapshot, logs, glyphs, width, terminal_height);
  }
  const size_t outer_width = width > 1 ? width - 1 : width;
  const size_t panel_width = outer_width > 4 ? outer_width - 4 : outer_width;
  const size_t panel_inner = panel_width > 2 ? panel_width - 2 : panel_width;
  const size_t panel_content_width = panel_inner > 2 ? panel_inner - 2 : panel_inner;
  const int max_body_lines = std::max(1, terminal_height - 2);

  std::vector<std::string> body;
  auto append = [&body](std::vector<std::string> lines) {
    body.insert(body.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
  };
  auto separator_with_glyphs = [&body, panel_width, &glyphs]() {
    body.push_back(std::string(kDim) + repeat_text(glyphs.horizontal, panel_width) + kReset);
  };

  append(make_panel(build_status_table(snapshot, panel_content_width), panel_width, snapshot.title, kWhiteOnDarkBlue, glyphs));
  separator_with_glyphs();
  append(make_panel({build_progress_row(snapshot, panel_content_width)}, panel_width, "", kWhiteOnGrey35, glyphs));
  if (static_cast<int>(body.size()) + 4 > max_body_lines) {
    return render_compact_screen(snapshot, logs, glyphs, width, terminal_height);
  }

  if (options.show_graph && graph) {
    std::vector<std::string> graph_panel =
        make_panel(build_graph_rows(*graph, panel_content_width, glyphs), panel_width, "Pipeline", kWhiteOnGrey19, glyphs);
    constexpr int kMinimumLogPanelLines = 3;
    if (static_cast<int>(body.size() + 1 + graph_panel.size() + 1 + kMinimumLogPanelLines) <= max_body_lines) {
      separator_with_glyphs();
      append(std::move(graph_panel));
    }
  }
  if (static_cast<int>(body.size()) + 4 > max_body_lines) {
    return render_compact_screen(snapshot, logs, glyphs, width, terminal_height);
  }
  if (static_cast<int>(body.size()) + 3 <= max_body_lines) {
    separator_with_glyphs();
    const int remaining_for_log_panel = max_body_lines - static_cast<int>(body.size());
    const int log_lines = std::min(std::max(1, options.log_lines), std::max(1, remaining_for_log_panel - 2));
    append(make_panel(build_log_rows(logs, log_lines), panel_width, "", kWhiteOnGrey23, glyphs));
  }

  std::ostringstream out;
  out << std::string(kDim) << glyphs.top_left << repeat_text(glyphs.horizontal, outer_width - 2) << glyphs.top_right
      << kReset << "\n";
  for (const std::string& line : body) {
    out << kDim << glyphs.vertical << kReset << " " << fit_visible(line, panel_width) << " " << kDim
        << glyphs.vertical << kReset << "\n";
  }
  out << kDim << glyphs.bottom_left << repeat_text(glyphs.horizontal, outer_width - 2) << glyphs.bottom_right << kReset
      << "\n";
  out << "\033[J";
  return out.str();
}

} // namespace

TerminalProgressUi::TerminalProgressUi(TerminalProgressOptions options) : options_(options) {
  options_.log_lines = std::max(1, options_.log_lines);
  options_.refresh_ms = std::max(100, options_.refresh_ms);
  options_.start_threshold = std::max(0, options_.start_threshold);
  glyphs_ = unicode_line_glyphs();
}

TerminalProgressUi::~TerminalProgressUi() {
  stop();
}

bool TerminalProgressUi::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) {
    return true;
  }

  std::cout << std::flush;
  std::cerr << std::flush;
  std::fflush(stdout);
  std::fflush(stderr);

  saved_stdout_fd_ = dup(STDOUT_FILENO);
  saved_stderr_fd_ = dup(STDERR_FILENO);
  capture_stdout_ = false;
  auto cleanup_failed_start = [this]() {
    restoreOutput();
    if (pipe_read_fd_ >= 0) {
      close(pipe_read_fd_);
      pipe_read_fd_ = -1;
    }
    if (saved_stdout_fd_ >= 0) {
      close(saved_stdout_fd_);
      saved_stdout_fd_ = -1;
    }
    if (saved_stderr_fd_ >= 0) {
      close(saved_stderr_fd_);
      saved_stderr_fd_ = -1;
    }
  };
  if (saved_stdout_fd_ < 0 || saved_stderr_fd_ < 0) {
    cleanup_failed_start();
    return false;
  }
  if (!isatty(saved_stderr_fd_)) {
    cleanup_failed_start();
    return false;
  }
  glyphs_ = terminal_line_glyphs(saved_stderr_fd_);

  if (options_.capture_output) {
    const bool capture_stdout = isatty(saved_stdout_fd_);
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
      cleanup_failed_start();
      return false;
    }
    pipe_read_fd_ = pipe_fds[0];
    if (dup2(pipe_fds[1], STDERR_FILENO) < 0 ||
        (capture_stdout && dup2(pipe_fds[1], STDOUT_FILENO) < 0)) {
      close(pipe_fds[1]);
      cleanup_failed_start();
      return false;
    }
    capture_stdout_ = capture_stdout;
    close(pipe_fds[1]);
  }

  started_ = true;
  stop_requested_ = false;
  dirty_ = true;
  update_count_ = 0;
  last_render_time_ = {};
  screen_started_ = true;
  writeToTerminal("\033[?1049h\033[?25l\033[2J\033[H");

  if (pipe_read_fd_ >= 0) {
    capture_thread_ = std::thread(&TerminalProgressUi::captureLoop, this);
  }
  render_thread_ = std::thread(&TerminalProgressUi::renderLoop, this);
  return true;
}

void TerminalProgressUi::stop() {
  bool should_stop = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    should_stop = started_;
    stop_requested_ = true;
    dirty_ = true;
  }
  if (!should_stop) {
    return;
  }

  std::cout << std::flush;
  std::cerr << std::flush;
  std::fflush(stdout);
  std::fflush(stderr);
  restoreOutput();

  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }
  if (render_thread_.joinable()) {
    render_thread_.join();
  }

  if (screen_started_) {
    writeToTerminal("\033[?25h\033[?1049l");
  }
  if (saved_stdout_fd_ >= 0) {
    close(saved_stdout_fd_);
    saved_stdout_fd_ = -1;
  }
  if (saved_stderr_fd_ >= 0) {
    close(saved_stderr_fd_);
    saved_stderr_fd_ = -1;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  started_ = false;
  screen_started_ = false;
  dirty_ = false;
  capture_stdout_ = false;
}

void TerminalProgressUi::restoreTerminalForInterrupt() {
  if (saved_stderr_fd_ >= 0) {
    constexpr const char* restore_sequence = "\033[?25h\033[?1049l";
    const char* data = restore_sequence;
    size_t remaining = std::strlen(restore_sequence);
    while (remaining > 0) {
      const ssize_t written = write(saved_stderr_fd_, data, remaining);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      data += written;
      remaining -= static_cast<size_t>(written);
    }
  }
  restoreOutput();
}

bool TerminalProgressUi::started() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return started_;
}

void TerminalProgressUi::update(TerminalProgressSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_ = std::move(snapshot);
  ++update_count_;
  requestRenderLocked();
}

void TerminalProgressUi::setGraphSnapshot(std::optional<TerminalProgressGraphSnapshot> graph) {
  std::lock_guard<std::mutex> lock(mutex_);
  graph_ = std::move(graph);
  requestRenderLocked();
}

void TerminalProgressUi::appendLogLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(mutex_);
  logs_.push_back(line);
  const size_t max_lines = static_cast<size_t>(std::max(100, options_.log_lines * 8));
  while (logs_.size() > max_lines) {
    logs_.pop_front();
  }
  requestRenderLocked();
}

std::string TerminalProgressUi::renderForTest(
    const TerminalProgressOptions& options,
    const TerminalProgressSnapshot& snapshot,
    const std::optional<TerminalProgressGraphSnapshot>& graph,
    const std::vector<std::string>& logs,
    int width,
    int height) {
  return render_screen(options, snapshot, graph, logs, unicode_line_glyphs(), width, height);
}

void TerminalProgressUi::captureLoop() {
  std::string partial;
  char buffer[4096];
  while (true) {
    const ssize_t n = read(pipe_read_fd_, buffer, sizeof(buffer));
    if (n > 0) {
      for (ssize_t i = 0; i < n; ++i) {
        const char ch = buffer[i];
        if (ch == '\n') {
          appendLogLine(partial);
          partial.clear();
        } else if (ch != '\r') {
          partial.push_back(ch);
        }
      }
      continue;
    }
    if (n == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    break;
  }
  if (!partial.empty()) {
    appendLogLine(partial);
  }
  if (pipe_read_fd_ >= 0) {
    close(pipe_read_fd_);
    pipe_read_fd_ = -1;
  }
}

void TerminalProgressUi::renderLoop() {
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool should_render = false;
    bool should_stop = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      should_stop = stop_requested_;
      if (dirty_ && shouldRenderScreenLocked()) {
        const auto now = std::chrono::steady_clock::now();
        if (last_render_time_ == std::chrono::steady_clock::time_point{} ||
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_render_time_).count() >=
                options_.refresh_ms ||
            should_stop) {
          dirty_ = false;
          last_render_time_ = now;
          should_render = true;
        }
      }
    }
    if (should_render) {
      renderOnce();
    }
    if (should_stop) {
      break;
    }
  }
}

void TerminalProgressUi::requestRenderLocked() {
  dirty_ = true;
}

void TerminalProgressUi::renderOnce() {
  const auto size = terminalSize();
  const std::string text = "\033[H" + render(size.first, size.second);
  writeToTerminal(text);
}

std::string TerminalProgressUi::render(int width, int height) const {
  TerminalProgressSnapshot snapshot;
  std::optional<TerminalProgressGraphSnapshot> graph;
  std::vector<std::string> logs;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = snapshot_;
    graph = graph_;
    logs.assign(logs_.begin(), logs_.end());
  }
  return render_screen(options_, snapshot, graph, logs, glyphs_, width, height);
}

bool TerminalProgressUi::shouldRenderScreenLocked() const {
  return started_ && update_count_ >= static_cast<uint64_t>(options_.start_threshold);
}

std::pair<int, int> TerminalProgressUi::terminalSize() const {
  struct winsize ws {};
  if (saved_stderr_fd_ >= 0 && ioctl(saved_stderr_fd_, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
    return {ws.ws_col, ws.ws_row};
  }
  return {120, 40};
}

void TerminalProgressUi::writeToTerminal(const std::string& text) const {
  const int fd = saved_stderr_fd_ >= 0 ? saved_stderr_fd_ : STDERR_FILENO;
  const char* data = text.data();
  size_t remaining = text.size();
  while (remaining > 0) {
    const ssize_t written = write(fd, data, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    data += written;
    remaining -= static_cast<size_t>(written);
  }
}

void TerminalProgressUi::restoreOutput() {
  if (capture_stdout_ && saved_stdout_fd_ >= 0) {
    dup2(saved_stdout_fd_, STDOUT_FILENO);
  }
  if (saved_stderr_fd_ >= 0) {
    dup2(saved_stderr_fd_, STDERR_FILENO);
  }
}

} // namespace hm
