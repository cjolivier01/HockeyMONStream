/**
 * @file gopro_remote_bridge.cpp
 * @brief Optional BLE bridge that maps GoPro Remote notifications to START/STOP.
 *
 * Uses SimpleBLE (if available) to subscribe to a GoPro Remote's notifiable
 * characteristic(s). Each notification toggles recording by sending START/STOP
 * to the dual-recordd Unix domain socket control interface.
 */
#ifdef HSTREAM_HAVE_SIMPLEBLE
#include <simpleble/SimpleBLE.h>
#endif

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

/** Default path for the control socket used to talk to dual-recordd. */
static const char* kDefaultSockPath = "/tmp/dual-record.sock";

/** Print usage information for the bridge. */
static void usage() {
  std::cerr << "Usage: gopro_remote_bridge [--sock PATH] [--remote-name SUBSTR] [--svc UUID --char UUID] [--list]" << std::endl;
}

/**
 * @brief Send a single command line (START/STOP/STATUS) to dual-recordd.
 * @return true if the command was written successfully.
 */
static bool send_cmd(const std::string& cmd, const char* sock_path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) { perror("socket"); return false; }
  sockaddr_un addr{}; addr.sun_family = AF_UNIX; strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path)-1);
  if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); close(fd); return false; }
  std::string line = cmd + "\n";
  if (write(fd, line.c_str(), line.size()) < 0) { perror("write"); close(fd); return false; }
  char buf[256]; read(fd, buf, sizeof(buf));
  close(fd);
  return true;
}

/**
 * @brief Entry point for the BLE-to-socket bridge.
 *
 * Without explicit service/char UUIDs, subscribes to all notifiable
 * characteristics and toggles START/STOP on any notification.
 */
int main(int argc, char** argv) {
  const char* sock_path = kDefaultSockPath;
  const char* remote_name_sub = "Remote";
  std::string svc_uuid;
  std::string char_uuid;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--sock")) {
      if (i + 1 < argc) sock_path = argv[++i]; else { usage(); return 1; }
    } else if (!strcmp(argv[i], "--remote-name")) {
      if (i + 1 < argc) remote_name_sub = argv[++i]; else { usage(); return 1; }
    } else if (!strcmp(argv[i], "--svc")) {
      if (i + 1 < argc) svc_uuid = argv[++i]; else { usage(); return 1; }
    } else if (!strcmp(argv[i], "--char")) {
      if (i + 1 < argc) char_uuid = argv[++i]; else { usage(); return 1; }
    } else if (!strcmp(argv[i], "--list")) {
      list_only = true;
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage(); return 0;
    }
  }

#ifndef HSTREAM_HAVE_SIMPLEBLE
  std::cerr << "gopro_remote_bridge built without SimpleBLE. Rebuild with -DHSTREAM_HAVE_SIMPLEBLE and SimpleBLE libs." << std::endl;
  return 1;
#else
  try {
    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) { std::cerr << "No BLE adapters found" << std::endl; return 1; }
    auto adapter = adapters[0];
    std::cout << "Scanning for GoPro remote..." << std::endl;
    adapter.scan_for(8000);
    auto results = adapter.scan_get_results();
    SimpleBLE::Peripheral remote;
    bool found = false;
    for (auto& p : results) {
      auto name = p.identifier();
      if (name.find("GoPro") != std::string::npos && name.find("Remote") != std::string::npos) {
        remote = p; found = true; break;
      }
    }
    if (!found) { std::cerr << "GoPro remote not found" << std::endl; return 1; }
    remote.connect();
    std::cout << "Connected to: " << remote.identifier() << std::endl;

    if (list_only) {
      for (auto& svc : remote.services()) {
        std::cout << "Service " << svc.uuid() << std::endl;
        for (auto& ch : svc.characteristics()) {
          std::cout << "  Char  " << ch.uuid() << " props:"
                    << (ch.can_read()?" R":"")
                    << (ch.can_write_request()?" W":"")
                    << (ch.can_write_command()?" w":"")
                    << (ch.can_notify()?" N":"")
                    << (ch.can_indicate()?" I":"")
                    << std::endl;
        }
      }
      return 0;
    }

    std::atomic<bool> started{false};
    int subs = 0;
    if (!svc_uuid.empty() && !char_uuid.empty()) {
      remote.notify(svc_uuid, char_uuid, [&](SimpleBLE::ByteArray /*bytes*/) {
        std::string cmd = started ? "STOP" : "START";
        send_cmd(cmd, sock_path);
        started = !started;
      });
      std::cout << "Subscribed to " << svc_uuid << " / " << char_uuid << std::endl;
      subs = 1;
    } else {
      for (auto& svc : remote.services()) {
        for (auto& ch : svc.characteristics()) {
          if (ch.can_notify()) {
            remote.notify(svc.uuid(), ch.uuid(), [&](SimpleBLE::ByteArray /*bytes*/) {
              std::string cmd = started ? "STOP" : "START";
              send_cmd(cmd, sock_path);
              started = !started;
            });
            ++subs;
          }
        }
      }
      std::cout << "Subscribed to " << subs << " notifiable characteristics (generic mode)." << std::endl;
    }

    std::cout << "Listening for remote notifications. Press Ctrl+C to exit." << std::endl;
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  } catch (const std::exception& e) {
    std::cerr << "BLE error: " << e.what() << std::endl; return 1;
  }
  return 0;
#endif
}
