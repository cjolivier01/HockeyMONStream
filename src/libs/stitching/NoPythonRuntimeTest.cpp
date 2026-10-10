#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trimmed(const std::string& line) {
  const auto first = line.find_first_not_of(" \t\r\n");
  return first == std::string::npos ? std::string() : line.substr(first, line.find_last_not_of(" \t\r\n") - first + 1);
}
} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> forbidden = {
      "python3",
      "HM_PYTHON",
      "PYTHONPATH",
      "setup_pretrained_assets.py",
      "hmlib.cli",
      "hmorientation",
      "hmcreate_control_points",
      "hmfind_ice_rink",
      "hmscoreboard"};
  bool ok = argc > 1;
  bool audited_int8_launcher = false;
  bool ui_binary_python = false;
  for (int i = 1; i < argc; ++i) {
    std::ifstream input(argv[i]);
    if (!input) {
      std::cerr << "FAIL: unable to inspect production runtime source " << argv[i] << '\n';
      ok = false;
      continue;
    }
    const std::string path = std::filesystem::path(argv[i]).lexically_normal().generic_string();
    char magic[4]{};
    input.read(magic, sizeof(magic));
    const bool elf = input.gcount() == 4 && std::string(magic, 4) == "\177ELF";
    input.clear();
    input.seekg(0);
    std::string line;
    size_t line_number = 0;
    while (std::getline(input, line)) {
      ++line_number;
      // The package builder contains a negative payload audit for these exact
      // strings. Keep that guard in scope without mistaking it for a launcher.
      if (line.find("grep -RIE") != std::string::npos)
        continue;
      for (const std::string& token : forbidden) {
        if (line.find(token) != std::string::npos) {
          if (token == "python3") {
            // Explicit source-checkout INT8 preparation is offline. Audit the
            // one launcher exactly; every other UI source stays in scope.
            if (ends_with(path, "src/apps/hstream-ui/Int8PreparationDialog.cpp") &&
                trimmed(line) == "process_.setProgram(env.value(\"HSTREAM_INT8_PYTHON\", \"python3\"));") {
              audited_int8_launcher = true;
              continue;
            }
            // The UI binary embeds that launcher's default interpreter name.
            // Continue checking all native model/calibration launcher tokens.
            if (elf && ends_with(path, "src/apps/hstream-ui/hstream-ui")) {
              ui_binary_python = true;
              continue;
            }
            // These package dependencies support the separate offline InStat
            // report tool, not native playback or runtime calibration.
            if (ends_with(path, "scripts/make_deb.sh") &&
                (trimmed(line) == "python3," || trimmed(line) == "python3-pil,"))
              continue;
          }
          std::cerr << "FAIL: production runtime source " << argv[i] << ':' << line_number
                    << " contains forbidden Python launcher token " << token << '\n';
          ok = false;
        }
      }
    }
  }
  if (ui_binary_python && !audited_int8_launcher) {
    std::cerr << "FAIL: UI interpreter literal requires the audited offline INT8 launcher source\n";
    ok = false;
  }
  return ok ? 0 : 1;
}
