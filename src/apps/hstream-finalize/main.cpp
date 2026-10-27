#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "hstream/src/libs/common/OutputRootConfig.h"
#include "hstream/src/libs/publication/Publication.h"

int main(int argc, char** argv) {
  try {
    namespace fs = std::filesystem;
    fs::path manifest, game, root;
    std::string id;
    bool telemetry_only = false;
    std::vector<std::string> command;
    std::vector<std::string> option_groups;
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--help") {
        std::cout << "Usage: hstream-finalize --manifest RUN_OR_RECEIPT.yaml\n"
                     "   or: hstream-finalize --game-dir DIR --run-id ID [--work-root DIR]\n"
                     "        [--options KEY=VALUE,...] [--telemetry-only] -- RUNNER ARGS...\n"
                     "Telemetry-only runs require a completed recording and forbid video archives.\n"
                     "Options mirror the runner's configuration overrides for working-root resolution only.\n"
                     "Publishes completed outputs and removes owned scratch files. Repeating a run ID resumes\n"
                     "publication/cleanup without encoding again. Failed encoding retains its working data.\n";
        return 0;
      }
      if (argument == "--") {
        while (++i < argc)
          command.emplace_back(argv[i]);
        break;
      }
      if (argument == "--telemetry-only") {
        telemetry_only = true;
        continue;
      }
      if (++i == argc)
        throw std::runtime_error("Missing value for " + argument);
      if (argument == "--manifest")
        manifest = argv[i];
      else if (argument == "--game-dir")
        game = argv[i];
      else if (argument == "--run-id")
        id = argv[i];
      else if (argument == "--work-root")
        root = argv[i];
      else if (argument == "--options")
        option_groups.emplace_back(argv[i]);
      else
        throw std::runtime_error("Unknown option: " + argument);
    }
    const auto progress = [](const std::string& message) { std::cout << message << std::endl; };
    if (!manifest.empty()) {
      if (!command.empty() || !game.empty() || !id.empty() || telemetry_only)
        throw std::runtime_error("Choose manifest or run mode");
      hm::publication::Finalize(manifest, progress);
      return 0;
    }
    if (game.empty() || id.empty() || command.empty())
      throw std::runtime_error("Game, run ID and runner are required");
    // An admitted run keeps its original workspace even if live configuration
    // has changed or can no longer be parsed. RunJob validates the saved binding.
    if (root.empty() && !fs::exists(hm::publication::ReceiptPath(fs::canonical(game), id))) {
      auto configured = hm::user_config::output_root_for_game(game, option_groups);
      if (!configured.ok())
        throw std::runtime_error(configured.status().ToString());
      root = *configured;
    }
    return hm::publication::RunJob(game, root, id, command, progress, telemetry_only);
  } catch (const std::exception& error) {
    std::cerr << "hstream-finalize: " << error.what() << '\n';
    return 1;
  }
}
