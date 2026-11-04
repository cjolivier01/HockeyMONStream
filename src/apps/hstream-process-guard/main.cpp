#include "hstream/src/libs/common/ProcessGuard.h"

#include <iostream>
#include <map>
#include <stdexcept>

int main(int argc, char** argv) {
  try {
    std::map<std::string, std::string> options;
    std::vector<std::string> command;
    int index = 1;
    for (; index < argc && std::string(argv[index]) != "--"; index += 2) {
      if (index + 1 >= argc || !options.emplace(argv[index], argv[index + 1]).second)
        throw std::runtime_error("Invalid process guard arguments");
    }
    if (index == argc || options.size() != 6)
      throw std::runtime_error("Process guard requires an exact command and receipt");
    for (++index; index < argc; ++index)
      command.emplace_back(argv[index]);
    const auto integer = [&](const char* key) {
      const auto& value = options.at(key);
      size_t used = 0;
      const auto number = std::stoll(value, &used);
      if (used != value.size())
        throw std::runtime_error("Invalid process guard timing");
      return number;
    };
    const auto result = hm::process_guard::Run(
        options.at("--receipt"),
        options.at("--token"),
        options.at("--nonce"),
        integer("--parent-pid"),
        integer("--grace-ms"),
        integer("--poll-ms"),
        command);
    if (result.ok())
      return *result;
    std::cerr << result.status() << '\n';
  } catch (const std::exception& error) {
    std::cerr << "hstream-process-guard: " << error.what() << '\n';
  }
  return 125;
}
