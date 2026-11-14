#include <iostream>
#include <string>
#include "hstream/src/libs/runtime_worker/Worker.h"

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--capabilities") {
      std::cout << hm::runtime_worker::Capabilities().Dump() << '\n';
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--request")
      return hm::runtime_worker::Run(argv[2]);
    std::cerr << "Usage: hstream-runtime-worker --request FILE | --capabilities\n";
  } catch (const std::exception& error) {
    std::cerr << "hstream-runtime-worker: " << error.what() << '\n';
  }
  return 125;
}
