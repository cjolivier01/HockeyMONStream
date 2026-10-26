#include "src/apps/hstream-ui/WebCameraExperiment.h"

#include <QtCore/QTemporaryDir>

#include <fstream>
#include <iostream>
#include <stdexcept>

int main() {
  try {
    using hm::playtracker_replay::TrialResult;
    using namespace hm::web;
    QTemporaryDir directory;
    if (!directory.isValid())
      throw std::runtime_error("Cannot create private catalog fixture");
    const auto catalog = std::filesystem::path(directory.path().toStdString()) / "catalog.json";
    const auto bundle = catalog.parent_path() / "recording";
    std::filesystem::create_directory(bundle);
    std::ofstream(bundle / "manifest.json") << "{}";
    ValidateLegacyCameraFiles(bundle / "manifest.json");
    std::ofstream(catalog.parent_path() / "outside.csv") << "outside recording";
    std::filesystem::create_symlink(catalog.parent_path() / "outside.csv", bundle / "tracks.csv");
    bool linked_companion_rejected = false;
    try {
      ValidateLegacyCameraFiles(bundle / "manifest.json");
    } catch (const std::exception&) {
      linked_companion_rejected = true;
    }
    if (!linked_companion_rejected)
      throw std::runtime_error("Legacy preparation accepted a linked companion outside its bundle");
    std::filesystem::remove(bundle / "tracks.csv");
    std::vector<TrialResult> trials(1);
    trials[0].name = "previous";
    const QJsonObject previous{{"views", QJsonArray{"previous"}}};
    WriteObject(catalog, previous);
    std::vector<TrialResult> paired(2);
    paired[0].name = "new native";
    paired[1].name = "new rink-aware";
    bool rejected = false;
    try {
      CommitCameraExperimentTrials(trials, paired, [&] {
        if (trials.size() != 3)
          throw std::runtime_error("Paired views were not published together");
        WriteObject(catalog, {{"views", QString(kMaximumResultBytes, QChar('x'))}});
      });
    } catch (const std::exception& error) {
      rejected = std::string(error.what()).find("size bound") != std::string::npos;
    }
    if (!rejected || trials.size() != 1 || trials.front().name != "previous" || ReadObject(catalog) != previous)
      throw std::runtime_error("Oversized catalog changed the existing views or published bytes");
    CommitCameraExperimentTrials(trials, std::move(paired), [&] {
      QJsonArray names;
      for (const auto& trial : trials)
        names.append(QString::fromStdString(trial.name));
      WriteObject(catalog, {{"views", names}});
    });
    if (trials.size() != 3 || ReadObject(catalog).value("views").toArray().size() != 3)
      throw std::runtime_error("A rejected publication prevented the next valid comparison");
    std::cout << "Camera comparison catalog rollback and retry passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
