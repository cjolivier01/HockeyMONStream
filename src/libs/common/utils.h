#pragma once

#include <gst/gst.h>
#include <yaml-cpp/yaml.h>

#include <stdio.h>

#include <iostream>
#include <functional>
#include <optional>

namespace hm {
class GPrintStreamBuffer : public std::streambuf {
 public:
  GPrintStreamBuffer() {}

 protected:
  // Buffer size
  static constexpr std::size_t bufferSize = 256;
  char buffer[bufferSize];

  // Overriding the overflow method
  int overflow(int c) override {
    if (c != EOF) {
      buffer[0] = static_cast<char>(c);
      buffer[1] = '\0';
      g_print("%s", buffer);
    }
    return c;
  }

  // Overriding sync method
  int sync() override {
    return 0; // Always successful
  }
};

class GPrintOStream : public std::ostream {
 public:
  GPrintOStream() : std::ostream(&gprintBuffer) {}

 private:
  GPrintStreamBuffer gprintBuffer;
};
extern GPrintOStream gout;

#define STRSIZE(str$) (sizeof(str$)/sizeof(str$[0]))

std::optional<std::pair<int, int>> extract_width_height(GstCaps* caps);

template <typename T>
inline void glist_visitor(GList *list, const std::function<void(T*)>& pred) {
  while (list) {
    pred(static_cast<T*>(list->data));
    list = list->next;
  }
}

YAML::Node deep_copy(const YAML::Node& node);

std::tuple<size_t, size_t> resize_to_fit(size_t origWidth, size_t origHeight, size_t maxWidth, size_t maxHeight);

#define TO_STRING(_stuff$) (std::stringstream() << _stuff$).str()

} // namespace hm
