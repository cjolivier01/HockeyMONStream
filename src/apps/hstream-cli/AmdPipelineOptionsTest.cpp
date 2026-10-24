#include "AmdPipelineOptions.h"

#include <iostream>
#include <stdexcept>

namespace {
void Check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
} // namespace
int main() {
  try {
    const auto parse = [](const std::vector<std::string>& extra) {
      std::vector<std::string> args{"hstream-cli", "-g", "game", "--enable-sinks=FAKE"};
      args.insert(args.end(), extra.begin(), extra.end());
      return hm::program::ParseAmdPipelineOptions(std::move(args));
    };
    for (const auto& extra : std::vector<std::vector<std::string>>{{"-t", "2"}, {"-t=2"}, {"--time-limit=00:02"}}) {
      auto result = parse(extra);
      Check(result.ok() && result->limit == 2000000000 && !result->start, "Duration forms differ");
    }
    auto result = parse({"--start-time=01:02.5", "-c", "app.yaml", "--options=a=1", "-o", "b=2"});
    Check(
        result.ok() && result->start == 62500000000 && result->application == "app.yaml" &&
            result->options == std::vector<std::string>{"a=1", "b=2"},
        "Launch options lost");
    result = parse({"--enable-sinks=RENDER,ENCODE_FILE", "--render-window-id=1234", "--render-realtime", "--show"});
    Check(
        result.ok() && result->window == 1234 && result->realtime && result->sinks.size() == 2,
        "Preview controls lost");
    for (const auto& extra : std::vector<std::vector<std::string>>{
             {"-c", "first.yaml", "-c", "second.yaml"},
             {"--cfg-file="},
             {"--enable-sinks=FAKE,"},
             {"--enable-sinks=FAKE,,RENDER"},
             {"--enable-sinks=FAKE,FAKE"},
             {"--enable-sinks=WEBRTC"},
             {"--enable-sources=URI"},
             {"--enable-sinks="},
             {"--render-window-id=-1"},
             {"--render-window-id=12"},
             {"--show"},
             {"--render-realtime"},
             {"--start-time=-1"},
             {"--start-time=nan"},
             {"--start-time=123garbage"},
             {"-t=0"},
             {"-t=0.0000000001"},
             {"--time-limit=9223372037"},
             {"--unknown"},
             {"positional"}})
      Check(!parse(extra).ok(), "Unsupported or malformed launch accepted");
    Check(!hm::program::ParseAmdPipelineOptions({"hstream-cli", "--enable-sinks=FAKE"}).ok(), "Missing game accepted");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
