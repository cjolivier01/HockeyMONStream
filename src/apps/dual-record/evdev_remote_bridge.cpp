/**
 * @file evdev_remote_bridge.cpp
 * @brief Optional Linux evdev bridge that maps input key presses to START/STOP.
 *
 * Many Bluetooth remotes (e.g., ATUMTEK) expose HID keyboard/media keys.
 * When connected, BlueZ routes notifications to the kernel HID driver, so
 * userland GATT may not see button events. Reading /dev/input/event* is
 * therefore more reliable for button handling.
 */

#include <linux/input.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <sstream>
#include <cctype>

/** Default path for the control socket used to talk to dual-recordd. */
static const char* kDefaultSockPath = "/tmp/dual-record.sock";

static volatile std::sig_atomic_t g_stop = 0;
static void on_sigint(int) { g_stop = 1; }

static void usage() {
  std::cerr << "Usage: evdev_remote_bridge [--sock PATH] [--dev /dev/input/eventX | --name SUBSTR]"
            << " [--key-start LIST] [--key-stop LIST] [--key-toggle LIST] [--list-devices] [--print-events]" << std::endl;
  std::cerr << "  LIST can be comma-separated KEY_* names (e.g., KEY_PLAYPAUSE) or numeric codes (e.g., 164,0xa4)." << std::endl;
}

/** Send a single START/STOP command line to dual-recordd. */
static bool send_cmd(const std::string& cmd, const char* sock_path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return false;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
  if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("connect");
    close(fd);
    return false;
  }
  std::string line = cmd + "\n";
  if (write(fd, line.c_str(), line.size()) < 0) {
    perror("write");
    close(fd);
    return false;
  }
  char buf[256];
  read(fd, buf, sizeof(buf));
  close(fd);
  return true;
}

static std::string evdev_name(int fd) {
  char name[256] = {0};
  if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) return std::string();
  return std::string(name);
}

static std::vector<std::string> list_event_nodes() {
  std::vector<std::string> nodes;
  DIR* dir = opendir("/dev/input");
  if (!dir) return nodes;
  struct dirent* ent;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    std::string name = ent->d_name;
    if (name.rfind("event", 0) == 0 || name.find("event") != std::string::npos) {
      nodes.emplace_back(std::string("/dev/input/") + name);
    }
  }
  closedir(dir);
  std::sort(nodes.begin(), nodes.end());
  return nodes;
}

int main(int argc, char** argv) {
  const char* sock_path = kDefaultSockPath;
  const char* name_sub = "ATUMTEK";
  const char* dev_path = nullptr;
  bool list_devices = false;
  bool print_events = false;

  std::unordered_set<int> keys_start;
  std::unordered_set<int> keys_stop;
  std::unordered_set<int> keys_toggle;

  auto to_upper = [](std::string s) {
    for (auto& c : s) c = std::toupper(static_cast<unsigned char>(c));
    return s;
  };

  static const std::unordered_map<std::string, int> kKeyMap = {
      {"KEY_PLAYPAUSE", KEY_PLAYPAUSE}, {"KEY_RECORD", KEY_RECORD},       {"KEY_ENTER", KEY_ENTER},
      {"KEY_SPACE", KEY_SPACE},         {"KEY_F13", KEY_F13},             {"KEY_F14", KEY_F14},
      {"KEY_F15", KEY_F15},             {"KEY_VOLUMEUP", KEY_VOLUMEUP},   {"KEY_VOLUMEDOWN", KEY_VOLUMEDOWN},
      {"KEY_CAMERA", KEY_CAMERA},       {"KEY_OK", KEY_OK},               {"KEY_POWER", KEY_POWER},
      {"KEY_PLAY", KEY_PLAY},           {"KEY_PAUSE", KEY_PAUSE},         {"KEY_STOP", KEY_STOP},
      {"KEY_NEXTSONG", KEY_NEXTSONG},   {"KEY_PREVIOUSSONG", KEY_PREVIOUSSONG},
  };

  auto key_name_to_code = [&](const std::string& token) -> int {
    // Try numeric
    if (!token.empty() && (std::isdigit(token[0]) || (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')))) {
      char* end = nullptr;
      long v = std::strtol(token.c_str(), &end, 0);
      if (end && *end == '\0' && v >= 0 && v <= 0x7fff) return static_cast<int>(v);
    }
    std::string k = token;
    k = to_upper(k);
    if (k.rfind("KEY_", 0) != 0) k = std::string("KEY_") + k;
    auto it = kKeyMap.find(k);
    if (it != kKeyMap.end()) return it->second;
    return -1;
  };

  auto parse_key_list = [&](const char* s, std::unordered_set<int>& out) {
    std::stringstream ss(s ? s : "");
    std::string tok;
    while (std::getline(ss, tok, ',')) {
      // trim spaces
      size_t b = tok.find_first_not_of(" \t");
      size_t e = tok.find_last_not_of(" \t");
      if (b == std::string::npos) continue;
      tok = tok.substr(b, e - b + 1);
      int code = key_name_to_code(tok);
      if (code >= 0) out.insert(code);
      else std::cerr << "Warning: unknown key token: " << tok << std::endl;
    }
  };

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--sock")) {
      if (i + 1 < argc) sock_path = argv[++i]; else { usage(); return 1; }
    } else if (!strcmp(argv[i], "--dev")) {
      if (i + 1 < argc) dev_path = argv[++i]; else { usage(); return 1; }
    } else if (!strcmp(argv[i], "--name")) {
      if (i + 1 < argc) name_sub = argv[++i]; else { usage(); return 1; }
    } else if (!strcmp(argv[i], "--list-devices")) {
      list_devices = true;
    } else if (!strcmp(argv[i], "--print-events")) {
      print_events = true;
    } else if (!strcmp(argv[i], "--key-start")) {
      if (i + 1 < argc) parse_key_list(argv[++i], keys_start); else { usage(); return 1; }
    } else if (!strcmp(argv[i], "--key-stop")) {
      if (i + 1 < argc) parse_key_list(argv[++i], keys_stop); else { usage(); return 1; }
    } else if (!strcmp(argv[i], "--key-toggle")) {
      if (i + 1 < argc) parse_key_list(argv[++i], keys_toggle); else { usage(); return 1; }
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage();
      return 0;
    }
  }

  std::signal(SIGINT, on_sigint);

  // List devices and exit if requested.
  auto nodes = list_event_nodes();
  if (list_devices) {
    for (const auto& n : nodes) {
      int fd = open(n.c_str(), O_RDONLY | O_NONBLOCK);
      if (fd < 0) continue;
      std::string nm = evdev_name(fd);
      close(fd);
      std::cout << n << ": " << nm << std::endl;
    }
    return 0;
  }

  std::optional<std::string> target_dev;
  if (dev_path) {
    target_dev = dev_path;
  } else {
    // Find first device whose name contains the substring.
    for (const auto& n : nodes) {
      int fd = open(n.c_str(), O_RDONLY | O_NONBLOCK);
      if (fd < 0) continue;
      std::string nm = evdev_name(fd);
      close(fd);
      if (!nm.empty() && nm.find(name_sub) != std::string::npos) { target_dev = n; break; }
    }
  }

  if (!target_dev) {
    std::cerr << "Input device not found. Use --list-devices to inspect." << std::endl;
    return 1;
  }

  int fd = open(target_dev->c_str(), O_RDONLY);
  if (fd < 0) {
    std::cerr << "Failed to open " << *target_dev << ": " << strerror(errno) << std::endl;
    if (errno == EACCES) std::cerr << "Hint: need permissions (try sudo or add user to input group)." << std::endl;
    return 1;
  }

  if (keys_start.empty() && keys_stop.empty() && keys_toggle.empty()) {
    // Default behavior: toggle on a few common keys.
    keys_toggle.insert(KEY_PLAYPAUSE);
    keys_toggle.insert(KEY_RECORD);
    keys_toggle.insert(KEY_ENTER);
    keys_toggle.insert(KEY_SPACE);
  }

  std::cout << "Listening on " << *target_dev << " (" << evdev_name(fd) << "). Press Ctrl+C to exit." << std::endl;

  bool started = false;
  while (!g_stop) {
    struct input_event ev;
    ssize_t r = read(fd, &ev, sizeof(ev));
    if (r < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) { usleep(1000); continue; }
      std::cerr << "read error: " << strerror(errno) << std::endl;
      break;
    }
    if (r != sizeof(ev)) continue;

    if (ev.type == EV_KEY) {
      if (print_events) {
        std::cout << "EV_KEY code=" << ev.code << " val=" << ev.value << std::endl;
      }
      if (ev.value == 1 /* press */) {
        if (!keys_start.empty() && keys_start.count(ev.code)) {
          send_cmd("START", sock_path);
          started = true;
          std::cout << "Key " << ev.code << ": START" << std::endl;
          continue;
        }
        if (!keys_stop.empty() && keys_stop.count(ev.code)) {
          send_cmd("STOP", sock_path);
          started = false;
          std::cout << "Key " << ev.code << ": STOP" << std::endl;
          continue;
        }
        if (!keys_toggle.empty() && keys_toggle.count(ev.code)) {
          const char* cmd = started ? "STOP" : "START";
          send_cmd(cmd, sock_path);
          started = !started;
          std::cout << "Key " << ev.code << ": " << cmd << std::endl;
          continue;
        }
      }
    }
  }

  close(fd);
  return 0;
}
