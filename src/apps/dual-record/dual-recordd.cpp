#include "dual_record_service.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>

#include <cstring>
#include <iostream>
#include <map>
#include <sstream>

static const char *kDefaultSockPath = "/tmp/dual-record.sock";
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int) { g_stop = 1; }

static std::map<std::string, std::string> parse_kv(const std::string &line) {
  std::map<std::string, std::string> kv;
  std::istringstream iss(line);
  std::string tok;
  while (iss >> tok) {
    auto pos = tok.find('=');
    if (pos != std::string::npos) {
      kv[tok.substr(0, pos)] = tok.substr(pos + 1);
    }
  }
  return kv;
}

static void apply_opts(const std::map<std::string, std::string> &kv, DualRecordOptions *o) {
  auto get = [&](const char *k) -> const char * { auto it = kv.find(k); return (it==kv.end())?nullptr:it->second.c_str(); };
  if (auto v = get("sensor0")) o->sensor0 = atoi(v);
  if (auto v = get("sensor1")) o->sensor1 = atoi(v);
  if (auto v = get("width")) o->width = atoi(v);
  if (auto v = get("height")) o->height = atoi(v);
  if (auto v = get("fps")) { o->fps_n = atoi(v); o->fps_d = 1; }
  if (auto v = get("bitrate")) o->bitrate_kbps = atoi(v);
  if (auto v = get("sensor_mode")) o->sensor_mode = atoi(v);
  if (auto v = get("duration")) o->duration_sec = atoi(v);
  if (auto v = get("out0")) o->out0 = v;
  if (auto v = get("out1")) o->out1 = v;
  if (auto v = get("out_dir")) o->out_dir = v;
  if (auto v = get("container")) o->container = v;
  if (auto v = get("sync")) o->filesink_sync = (!strcasecmp(v, "true") || !strcmp(v, "1"));
  if (auto v = get("auto_30fps")) o->auto_30fps = (!strcasecmp(v, "true") || !strcmp(v, "1"));
  if (auto v = get("exposure0_us")) o->exposure0_us = atoi(v);
  if (auto v = get("exposure1_us")) o->exposure1_us = atoi(v);
  if (auto v = get("gain0")) o->gain0 = atof(v);
  if (auto v = get("gain1")) o->gain1 = atof(v);
}

static void daemonize_if_needed(bool debug_mode) {
  if (debug_mode) return;
  pid_t pid = fork();
  if (pid < 0) _exit(1);
  if (pid > 0) _exit(0); // parent exits
  if (setsid() < 0) _exit(1);
  signal(SIGHUP, SIG_IGN);
  pid = fork();
  if (pid < 0) _exit(1);
  if (pid > 0) _exit(0);
  umask(0);
  chdir("/");
  int fd = open("/dev/null", O_RDWR);
  if (fd >= 0) {
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > 2) close(fd);
  }
}

int main(int argc, char** argv) {
  bool debug_mode = false;
  const char* sock_path = kDefaultSockPath;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--debug")) {
      debug_mode = true;
    } else if (!strcmp(argv[i], "--sock") && i + 1 < argc) {
      sock_path = argv[++i];
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      std::cerr << "Usage: dual-recordd [-d|--debug] [--sock /path.sock]\n";
      return 0;
    }
  }

  daemonize_if_needed(debug_mode);

  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  // Prepare UDS server
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) { perror("socket"); return 1; }
  sockaddr_un addr{}; addr.sun_family = AF_UNIX; strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path)-1);
  unlink(sock_path);
  if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
  if (listen(fd, 4) < 0) { perror("listen"); return 1; }

  gst_init(nullptr, nullptr);
  DualRecorderService svc;

  if (debug_mode) std::cerr << "dual-recordd listening on " << sock_path << std::endl;
  for (; !g_stop; ) {
    int cfd = accept(fd, nullptr, nullptr);
    if (cfd < 0) { if (errno == EINTR && g_stop) break; perror("accept"); continue; }
    char buf[4096]; ssize_t n = read(cfd, buf, sizeof(buf)-1);
    if (n <= 0) { close(cfd); continue; }
    buf[n] = 0; std::string line(buf);

    // Split at first space
    std::string cmd; std::string rest;
    auto sp = line.find(' ');
    if (sp == std::string::npos) { cmd = line; }
    else { cmd = line.substr(0, sp); rest = line.substr(sp+1); }
    // Trim newlines
    while (!cmd.empty() && (cmd.back()=='\n' || cmd.back()=='\r')) cmd.pop_back();

    std::string reply;
    if (cmd == "START") {
      auto kv = parse_kv(rest);
      DualRecordOptions opt;
      apply_opts(kv, &opt);
      std::string err;
      if (svc.Start(opt, &err)) reply = "OK\n";
      else { reply = std::string("ERROR ") + err + "\n"; }
    } else if (cmd == "STOP") {
      std::string err;
      if (svc.Stop(&err)) reply = "OK\n"; else reply = std::string("ERROR ") + err + "\n";
    } else if (cmd == "STATUS") {
      reply = svc.Status(); reply += "\n";
    } else {
      reply = "ERROR unknown command\n";
    }

    write(cfd, reply.c_str(), reply.size());
    close(cfd);
  }
  close(fd);
  unlink(sock_path);
  return 0;
}
