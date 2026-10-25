#include "Protocol.h"

#include <csignal>
#include <iostream>
#include <string>

namespace wire = hm::model_infer;
namespace {
void Expect(bool condition) {
  if (!condition)
    throw std::runtime_error("Protocol assertion failed");
}
template <class Function>
void Fails(Function function) {
  bool failed = false;
  try {
    function();
  } catch (const std::runtime_error&) {
    failed = true;
  }
  Expect(failed);
}
struct Pipe {
  int ends[2];
  Pipe() {
    Expect(::pipe(ends) == 0);
  }
  ~Pipe() {
    for (int fd : ends)
      if (fd >= 0)
        ::close(fd);
  }
  void Finish() {
    ::close(ends[1]);
    ends[1] = -1;
  }
};
} // namespace
int main() {
  try {
    for (uint32_t count : {0u, 1u, 32u}) {
      Pipe pipe;
      wire::WriteBatch(pipe.ends[1], count);
      pipe.Finish();
      Expect(wire::ReadBatch(pipe.ends[0]) == count);
      Expect(wire::ReadBatch(pipe.ends[0]) == 0);
    }
    for (uint32_t count : {33u, UINT32_MAX}) {
      Pipe pipe;
      wire::WriteBatch(pipe.ends[1], count);
      Fails([&] { wire::ReadBatch(pipe.ends[0]); });
    }
    for (size_t length : {1u, 2u, 3u}) {
      Pipe pipe;
      const char header[4] = {1, 0, 0, 0};
      wire::WriteExact(pipe.ends[1], header, length);
      pipe.Finish();
      Fails([&] { wire::ReadBatch(pipe.ends[0]); });
    }
    {
      Pipe pipe;
      wire::WriteExact(pipe.ends[1], "short", 5);
      pipe.Finish();
      char output[6];
      Fails([&] { wire::ReadExact(pipe.ends[0], output, sizeof(output)); });
    }
    {
      Pipe pipe;
      ::close(pipe.ends[0]);
      pipe.ends[0] = -1;
      std::signal(SIGPIPE, SIG_IGN);
      Fails([&] { wire::WriteBatch(pipe.ends[1], 1); });
    }
    wire::Finite({0, -1, 1});
    Fails([&] { wire::Finite({std::numeric_limits<float>::infinity()}); });
    Fails([&] { wire::Finite({std::numeric_limits<float>::quiet_NaN()}); });
    std::cout << "PASS bounded binary inference protocol, EOF, truncation, closed response and finite tensors\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
