#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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
  for (int i = 1; i < argc; ++i) {
    std::ifstream input(argv[i]);
    if (!input) {
      std::cerr << "FAIL: unable to inspect production runtime source " << argv[i] << '\n';
      ok = false;
      continue;
    }
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
          std::cerr << "FAIL: production runtime source " << argv[i] << ':' << line_number
                    << " contains forbidden Python launcher token " << token << '\n';
          ok = false;
        }
      }
    }
  }
  return ok ? 0 : 1;
}
