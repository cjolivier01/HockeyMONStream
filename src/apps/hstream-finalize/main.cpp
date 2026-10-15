#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "hstream/src/libs/common/UserConfig.h"
#include "hstream/src/libs/publication/Publication.h"

int main(int argc, char** argv) {
  try {
    namespace fs = std::filesystem;
    fs::path manifest, game, root;
    std::string id;
    std::vector<std::string> command;
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--help") {
        std::cout << "Usage: hstream-finalize --manifest RUN_OR_RECEIPT.yaml\n"
                     "   or: hstream-finalize --game-dir DIR --run-id ID [--work-root DIR] -- RUNNER ARGS...\n"
                     "Publishes completed outputs and removes owned scratch files. Repeating a run ID resumes\n"
                     "publication/cleanup without encoding again. Failed encoding retains its working data.\n";
        return 0;
      }
      if (argument == "--") {
        while (++i < argc)
          command.emplace_back(argv[i]);
        break;
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
      else
        throw std::runtime_error("Unknown option: " + argument);
    }
    const auto progress = [](const std::string& message) { std::cout << message << std::endl; };
    if (!manifest.empty()) {
      if (!command.empty() || !game.empty() || !id.empty())
        throw std::runtime_error("Choose manifest or run mode");
      hm::publication::Finalize(manifest, progress);
      return 0;
    }
    if (game.empty() || id.empty() || command.empty())
      throw std::runtime_error("Game, run ID and runner are required");
    if (root.empty()) {
      auto config = hm::user_config::load_or_create();
      if (!config.ok())
        throw std::runtime_error(config.status().ToString());
      auto configured = hm::user_config::output_root(*config);
      if (!configured.ok())
        throw std::runtime_error(configured.status().ToString());
      root = *configured;
    }
    return hm::publication::RunJob(game, root, id, command, progress);
  } catch (const std::exception& error) {
    std::cerr << "hstream-finalize: " << error.what() << '\n';
    return 1;
  }
}
