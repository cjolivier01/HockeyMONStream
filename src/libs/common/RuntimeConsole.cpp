#include "hstream/src/libs/common/RuntimeConsole.h"

#include <cstdio>
#include <cstring>

namespace hm {

void WriteRuntimeConsole(const char* message) {
  if (!message)
    return;
  if (std::strncmp(message, "HSTREAM_", 8) != 0) {
    std::fputs(message, stdout);
    std::fflush(stdout);
    return;
  }
  // GStreamer may print a warning in several stderr calls. Even a single
  // stdout write can then begin inside that warning's unfinished line.
  // Lock stderr before stdout, flush older fragments, and frame the entire
  // protocol record while both streams are held, including large JSON graphs.
  ::flockfile(stderr);
  ::flockfile(stdout);
  std::fflush(stderr);
  std::fflush(stdout);
  std::fputc('\n', stdout);
  std::fputs(message, stdout);
  const size_t length = std::strlen(message);
  if (length && message[length - 1] != '\n')
    std::fputc('\n', stdout);
  std::fflush(stdout);
  ::funlockfile(stdout);
  ::funlockfile(stderr);
}

} // namespace hm
