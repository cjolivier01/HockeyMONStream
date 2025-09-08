#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <sstream>

static const char *kDefaultSockPath = "/tmp/dual-record.sock";

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: dualctl [-s|--sock PATH] <START|STOP|STATUS> [key=value ...]\\n";
    return 1;
  }
  const char* sock_path = kDefaultSockPath;
  int idx = 1;
  for (; idx < argc; ++idx) {
    if (!strcmp(argv[idx], "-s") || !strcmp(argv[idx], "--sock")) {
      if (idx + 1 < argc) { sock_path = argv[++idx]; continue; }
      std::cerr << "Missing value for --sock" << std::endl; return 1;
    } else {
      break;
    }
  }
  if (idx >= argc) { std::cerr << "Missing command" << std::endl; return 1; }
  std::ostringstream oss;
  oss << argv[idx++];
  for (int i = idx; i < argc; ++i) oss << ' ' << argv[i];
  oss << '\\n';
  std::string msg = oss.str();

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) { perror("socket"); return 1; }
  sockaddr_un addr{}; addr.sun_family = AF_UNIX; strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path)-1);
  if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); return 1; }
  if (write(fd, msg.c_str(), msg.size()) < 0) { perror("write"); return 1; }
  char buf[4096]; ssize_t n = read(fd, buf, sizeof(buf)-1); if (n < 0) { perror("read"); return 1; }
  buf[n] = 0; std::cout << buf;
  close(fd);
  return 0;
}

