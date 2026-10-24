#include "hstream/src/libs/common/ConfigLayers.h"

#include <unistd.h>

#include <fstream>
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
    const auto baseline = YAML::Load("rink: {camera: {width: 1, height: 2}}\npipeline: {pose: {rate: 10}}\nkeep: true");
    const auto user = YAML::Load("rink: {camera: {width: 3}}\npipeline: {detector: {config-file: user.yaml}}");
    const auto game = YAML::Load("rink: {camera: {height: 4}}\npipeline: {pose: {rate: 20}}\nempty: {}\nlist: [1, 2]");
    const auto app =
        YAML::Load("detector: {config-file: shipped.yaml}\npose: {rate: 5, enabled: true}\nsource: {enable: 1}");
    const auto b = YAML::Dump(baseline), u = YAML::Dump(user), g = YAML::Dump(game), a = YAML::Dump(app);
    auto resolved = hm::config::ResolveProgramConfiguration(
        baseline,
        user,
        game,
        app,
        {"rink.camera.width=7,rink.camera.width=8,pipeline.detector.config-file=first.yaml",
         "pipeline.detector.config-file=final.yaml"});
    Check(resolved.ok(), "Cannot resolve valid layers");
    Check(
        YAML::Dump(baseline) == b && YAML::Dump(user) == u && YAML::Dump(game) == g && YAML::Dump(app) == a,
        "Configuration inputs were mutated");
    const auto& v = resolved->values;
    Check(
        v["rink"]["camera"]["width"].as<int>() == 7 && v["rink"]["camera"]["height"].as<int>() == 4,
        "Canonical overlays or first-wins option groups changed");
    Check(
        v["pipeline"]["detector"]["config-file"].as<std::string>() == "final.yaml" &&
            v["pipeline"]["pose"]["rate"].as<int>() == 20 && v["pipeline"]["pose"]["enabled"].as<bool>() &&
            v["pipeline"]["source"]["enable"].as<int>() == 1 && v["keep"].as<bool>(),
        "Native underlay or later option groups changed");
    Check(
        resolved->ranks.at("rink.camera.width") == 3 && resolved->ranks.at("rink.camera.height") == 2 &&
            resolved->ranks.at("pipeline.detector.config-file") == 3 && resolved->ranks.at("empty") == 2 &&
            resolved->ranks.at("list") == 2 && !resolved->ranks.count("keep") &&
            !resolved->ranks.count("pipeline.source.enable"),
        "Explicit layer provenance changed");
    // Result ownership must not leak aliases back into caller snapshots.
    resolved->values["list"][0] = 99;
    resolved->values["pipeline"]["source"]["enable"] = 0;
    Check(YAML::Dump(game) == g && YAML::Dump(app) == a, "Resolved child nodes alias input documents");
    for (const auto* invalid : {"broken", "key=value=more", "=value", "key=value,"})
      Check(
          !hm::config::ResolveProgramConfiguration(baseline, user, game, app, {invalid}).ok(),
          "Malformed option group accepted");
    Check(
        !hm::config::ResolveProgramConfiguration(YAML::Node(), user, game, app, {}).ok(), "Missing baseline accepted");
    const auto replacement = YAML::Load("rink: [1, 2]");
    auto sequence = hm::config::ResolveProgramConfiguration(baseline, user, replacement, app, {});
    Check(
        sequence.ok() && sequence->values["rink"].IsSequence() && sequence->values["rink"].size() == 2,
        "Sequence overlay merged as a map");
    const auto null_pipeline = YAML::Load("pipeline: null");
    auto native = hm::config::ResolveProgramConfiguration(baseline, user, null_pipeline, app, {});
    Check(
        native.ok() && native->values["pipeline"]["pose"]["rate"].as<int>() == 5,
        "Structural underlay no longer supplies a replaced pipeline map");
    std::string temporary = (std::filesystem::temp_directory_path() / "hstream-config-roots-XXXXXX").string();
    Check(mkdtemp(temporary.data()), "Cannot create configuration fixture");
    const std::filesystem::path root(temporary);
    hm::config::ReferenceRoots roots{root / "app", root / "baseline", root / "game", root / "user", root / "cwd"};
    for (const auto& dir : {roots.application, roots.baseline, roots.game, roots.user, roots.working}) {
      std::filesystem::create_directories(dir);
      std::ofstream(dir / "camera.yaml") << "{}\n";
    }
    for (int rank = 0; rank <= 3; ++rank) {
      const auto found = hm::config::ResolveReference("camera.yaml", rank, roots, "camera");
      const auto expected = rank == 3 ? roots.working
          : rank == 2                 ? roots.game
          : rank == 1                 ? roots.user
                                      : roots.application;
      Check(found.ok() && *found == expected / "camera.yaml", "Reference ignored its declaring layer");
    }
    const auto absolute = hm::config::ResolveReference(roots.baseline / "camera.yaml", 3, roots, "camera");
    Check(absolute.ok() && *absolute == roots.baseline / "camera.yaml", "Absolute reference searched other layers");
    Check(
        !hm::config::ResolveReference(root / "missing" / "camera.yaml", 3, roots, "camera").ok(),
        "Missing absolute reference fell back to another file");
    std::filesystem::remove_all(root);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
