#include "hstream/src/libs/common/RuntimeConsole.h"

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

int main() {
  int output[2];
  if (::pipe(output) != 0)
    return 1;
  const std::string record = "HSTREAM_PREVIEW_PRESENTATION {\"payload\":\"" + std::string(32768, 'x') + "\"}";
  const pid_t child = ::fork();
  if (child < 0)
    return 1;
  if (child == 0) {
    ::close(output[0]);
    if (::dup2(output[1], STDOUT_FILENO) < 0 || ::dup2(output[1], STDERR_FILENO) < 0)
      ::_exit(2);
    ::close(output[1]);
    hm::WriteRuntimeConsole(nullptr);
    hm::WriteRuntimeConsole("ordinary-");
    hm::WriteRuntimeConsole("fragment\n");
    std::fputs("unfinished SDK warning: ", stderr);
    hm::WriteRuntimeConsole("HSTREAM_PREVIEW_PRESENTATION {\"version\":1}\n");
    std::fputs("warning continuation\n", stderr);
    std::atomic<bool> stop{false};
    std::thread warnings([&] {
      while (!stop.load()) {
        std::fputs("SDK warning fragment ", stderr);
        std::this_thread::yield();
        std::fputs("continued\n", stderr);
      }
    });
    for (int index = 0; index < 100; ++index)
      hm::WriteRuntimeConsole(record.c_str());
    stop = true;
    warnings.join();
    ::_exit(0);
  }
  ::close(output[1]);
  std::string captured;
  char buffer[16384];
  for (;;) {
    const ssize_t count = ::read(output[0], buffer, sizeof(buffer));
    if (count > 0)
      captured.append(buffer, static_cast<size_t>(count));
    else if (count == 0)
      break;
    else if (errno != EINTR)
      return 1;
  }
  ::close(output[0]);
  int status = 0;
  if (::waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return 1;
  if (captured.find("ordinary-fragment\nunfinished SDK warning: \nHSTREAM_PREVIEW_PRESENTATION "
                    "{\"version\":1}\nwarning continuation\n") != 0) {
    std::cerr << "Protocol record was not separated from fragmented diagnostics\n";
    return 1;
  }
  std::istringstream lines(captured);
  std::string line;
  int records = 0;
  while (std::getline(lines, line)) {
    if (line == record)
      ++records;
  }
  if (records != 100) {
    std::cerr << "Concurrent diagnostics interrupted a protocol record\n";
    return 1;
  }
  return 0;
}
