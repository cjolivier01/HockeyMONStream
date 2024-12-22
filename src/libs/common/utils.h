#pragma once

#include <gst/gst.h>
#include <iostream>

#include <stdio.h>

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

} // namespace hm
