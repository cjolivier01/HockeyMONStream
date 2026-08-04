#include <fstream>
#include <iostream>
#include <sstream>
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
    std::ostringstream contents;
    contents << input.rdbuf();
    for (const std::string& token : forbidden) {
      if (contents.str().find(token) != std::string::npos) {
        std::cerr << "FAIL: production runtime source " << argv[i] << " contains forbidden Python launcher token "
                  << token << '\n';
        ok = false;
      }
    }
  }
  return ok ? 0 : 1;
}
