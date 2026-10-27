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
    auto paged = catalog.parent_path() / "paged";
    std::filesystem::create_directory(paged);
    std::vector<hm::playtracker_replay::Frame> trajectory(18000);
    auto overlay = std::make_shared<hm::preview_overlay::OverlaySnapshot>();
    overlay->coordinate_width = 13802;
    overlay->player_rects.resize(55);
    for (auto& player : overlay->player_rects) {
      player.rect.left = 1234.5678;
      player.rect.width = 15.25;
    }
    for (auto& frame : trajectory)
      frame.overlays = overlay;
    std::vector<CameraExperimentCatalogView> views(2, {"Comparison", &trajectory});
    std::size_t published = 0;
    std::uint64_t revision = 0;
    PublishCameraExperimentCatalog(paged, "run", {}, views, published, revision);
    std::uintmax_t total = 0;
    const auto manifest = ReadObject(paged / "camera-view-0.json");
    std::size_t frame_count = 0, points = 0;
    for (const auto& value : manifest.value("chunks").toArray()) {
      const auto path = paged / value.toObject().value("artifact").toObject().value("path").toString().toStdString();
      const auto size = std::filesystem::file_size(path);
      total += size;
      const auto items = ReadObject(path).value("items").toArray();
      if (size > kCameraChunkBytes || items.size() > kCameraChunkItems)
        throw std::runtime_error("Camera frame chunk exceeds its transport bound");
      for (const auto& item : items) {
        frame_count += item.toObject().value("last").toBool();
        points += item.toObject().value("players").toArray().size();
      }
    }
    if (total * 2 <= kMaximumResultBytes || published != 2 || revision != 1 || frame_count != 18000 || points != 990000)
      throw std::runtime_error("Dense admitted trajectories were bounded, lost or duplicated");
    paged = catalog.parent_path() / "history";
    std::filesystem::create_directory(paged);
    std::vector<hm::playtracker_replay::Frame> small(1);
    views.assign(2, {"Comparison", &small});
    published = 0;
    revision = 0;
    PublishCameraExperimentCatalog(paged, "run", {}, views, published, revision);
    const auto first_time = std::filesystem::last_write_time(paged / "camera-view-0.json");
    views.resize(142, {"Small comparison", &small});
    PublishCameraExperimentCatalog(paged, "run", {}, views, published, revision);
    const auto paged_catalog = ReadObject(paged / "camera-experiment-catalog.json");
    if (published != 142 || revision != 2 || paged_catalog.value("view_count").toInt() != 142 ||
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
    if (std::filesystem::exists(paged / "camera-view-142.json") ||
        std::filesystem::exists(paged / "camera-view-142-chunk-0.json") ||
        std::filesystem::exists(paged / "camera-history-128-143.json"))
      throw std::runtime_error("Failed batch retained uncommitted files");
    small.front().sample_id = 123;
    PublishCameraExperimentCatalog(paged, "run", {}, views, published, revision);
    const auto retried = ReadObject(paged / "camera-view-142-chunk-0.json").value("items").toArray();
    if (published != 143 || revision != 3 ||
        retried.at(0).toObject().value("value").toObject().value("sample").toString() != "123")
      throw std::runtime_error("Retry retained an uncommitted trajectory from the failed batch");
    const auto fragmented = catalog.parent_path() / "fragmented";
    std::filesystem::create_directory(fragmented);
    auto many = std::make_shared<hm::preview_overlay::OverlaySnapshot>(*overlay);
    many->player_rects.resize(400000, overlay->player_rects.front());
    std::vector<hm::playtracker_replay::Frame> one(1);
    one[0].overlays = many;
    std::size_t fragment_views = 0;
    std::uint64_t fragment_revision = 0;
    PublishCameraExperimentCatalog(
        fragmented, "run", {}, {{"Recorded", &one}, {"Baseline", &one}}, fragment_views, fragment_revision);
    const auto fragments = ReadObject(fragmented / "camera-view-0.json").value("chunks").toArray();
    points = 0;
    frame_count = 0;
    for (const auto& value : fragments) {
      const auto path =
          fragmented / value.toObject().value("artifact").toObject().value("path").toString().toStdString();
      if (std::filesystem::file_size(path) > kCameraChunkBytes)
        throw std::runtime_error("A single dense frame exceeded its chunk bound");
      for (const auto& value : ReadObject(path).value("items").toArray()) {
        const auto item = value.toObject();
        if (item.value("offset").toInt() != int(points))
          throw std::runtime_error("A dense frame fragment lost its exact player offset");
        points += item.value("players").toArray().size();
        frame_count += item.value("last").toBool();
      }
    }
    if (fragments.size() < 2 || points != 400000 || frame_count != 1)
      throw std::runtime_error("Dense frame fragmentation lost or duplicated values");
    std::cout << "Camera comparison catalog chunks, rollback and retry passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
