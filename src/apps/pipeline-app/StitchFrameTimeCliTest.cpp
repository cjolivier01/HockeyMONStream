#include <iostream>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

bool exits_with_argument_error(const char* executable, const char* value) {
  const pid_t child = ::fork();
  if (child == 0) {
    std::vector<char*> args = {
        const_cast<char*>(executable),
        const_cast<char*>(value),
        nullptr,
    };
    ::execv(executable, args.data());
    _exit(127);
  }
  int status = 0;
  return child > 0 && ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) != 0 &&
      WEXITSTATUS(status) != 127;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "FAIL: expected hstream-cli path\n";
    return 1;
  }
  for (const char* value : {
           "--stitch-frame-time=bogus",
           "--stitch-frame-time=00:00:07junk",
           "--stitch-frame-time=-1",
           "--stitch-frame-time=00:60:00",
           "--stitch-frame-time=inf",
       }) {
    if (!exits_with_argument_error(argv[1], value)) {
      std::cerr << "FAIL: malformed stitch-frame time did not produce a normal nonzero exit: " << value << '\n';
      return 1;
    }
  }
  return 0;
}
