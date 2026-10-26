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
    const auto paged = catalog.parent_path() / "paged";
    std::filesystem::create_directory(paged);
    std::vector<hm::playtracker_replay::Frame> trajectory(50000);
    std::vector<CameraExperimentCatalogView> views(5, {"Comparison", &trajectory});
    std::size_t published = 0;
    std::uint64_t revision = 0;
    PublishCameraExperimentCatalog(paged, "run", {}, views, published, revision);
    std::uintmax_t total = 0;
    for (int view = 0; view < 5; ++view)
      total += std::filesystem::file_size(paged / ("camera-view-" + std::to_string(view) + ".json"));
    if (total <= kMaximumResultBytes || published != 5 || revision != 1)
      throw std::runtime_error("Separate trajectories retained the cumulative catalog size limit");
    const auto first_time = std::filesystem::last_write_time(paged / "camera-view-0.json");
    std::vector<hm::playtracker_replay::Frame> small(1);
    views.resize(142, {"Small comparison", &small});
    PublishCameraExperimentCatalog(paged, "run", {}, views, published, revision);
    const auto paged_catalog = ReadObject(paged / "camera-experiment-catalog.json");
    if (published != 142 || revision != 2 || paged_catalog.value("views").toArray().size() != 142 ||
        std::filesystem::last_write_time(paged / "camera-view-0.json") != first_time)
      throw std::runtime_error("Appending trial 140 rewrote an immutable view or lost history");
    views.push_back({"Rejected comparison", &small});
    bool failed_catalog = false;
    try {
      PublishCameraExperimentCatalog(
          paged, "run", {{"oversize", QString(kMaximumResultBytes, QChar('x'))}}, views, published, revision);
    } catch (const std::exception&) {
      failed_catalog = true;
    }
    if (!failed_catalog || published != 142 || revision != 2 ||
        ReadObject(paged / "camera-experiment-catalog.json") != paged_catalog)
      throw std::runtime_error("Failed publication changed the catalog revision or committed view count");
    small.front().sample_id = 123;
    PublishCameraExperimentCatalog(paged, "run", {}, views, published, revision);
    const auto retried = ReadObject(paged / "camera-view-142.json").value("frames").toArray();
    if (published != 143 || revision != 3 || retried.at(0).toObject().value("sample").toString() != "123")
      throw std::runtime_error("Retry retained an uncommitted trajectory from the failed batch");
    std::cout << "Camera comparison catalog rollback and retry passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
