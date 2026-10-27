#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/stitching/CalibrationMatches.h"
#include "src/apps/hstream-ui/WebJobSpec.h"

#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
template <typename T>
T take(absl::StatusOr<T> value) {
  if (!value.ok())
    throw std::runtime_error(value.status().ToString());
  return std::move(*value);
}
void config(const fs::path& game, const YAML::Node& node) {
  std::ofstream(game / "config.yaml") << YAML::Dump(node);
}
QJsonObject invoke(
    const QString& binary,
    const fs::path& root,
    const QString& operation,
    const QJsonObject& payload,
    bool success = true,
    const QJsonObject& runtime = {}) {
  fs::remove(root / "operation-result.json");
  fs::remove(root / "operation-data.json");
  const auto path = root / "operation-request.json";
  hm::web::WriteObject(
      path,
      {{"schema", 1},
       {"run_id", "matches-contract"},
       {"operation", operation},
       {"root", QString::fromStdString(root.string())},
       {"game_dir", "game"},
       {"payload", payload},
       {"runtime", runtime}});
  QProcess child;
  child.start(binary, {"--request", QString::fromStdString(path.string())});
  require(child.waitForFinished(30000), "Match adapter timed out");
  if ((child.exitCode() == 0) != success) {
    std::cerr << child.readAllStandardError().constData();
    throw std::runtime_error("Unexpected match adapter outcome");
  }
  return hm::web::ReadObject(root / "operation-result.json");
}

void seed_empty_matches(const fs::path& game) {
  auto document = YAML::LoadFile((game / "config.yaml").string());
  hm::stitching::CalibrationMatchSet empty;
  empty.matcher = hm::stitching::ControlPointMatcher::kAkazeHamming;
  empty.source_context = take(hm::stitching::CalibrationMatchSourceContext(document, game));
  for (int pair = 0; pair < document["hstream_ui"]["stitching_calibration"]["frame_count"].as<int>(); ++pair) {
    hm::stitching::CalibrationMatchFrame frame;
    for (int camera = 0; camera < 2; ++camera) {
      frame.images[camera] = game / (std::to_string(pair) + (camera ? "right.png" : "left.png"));
      frame.sizes[camera] = {80, 60};
      frame.source_paths[camera] = (game / (camera ? "right.mp4" : "left.mp4")).string();
      frame.source_seconds[camera] = pair;
      QImage image(80, 60, QImage::Format_RGB888);
      image.fill(Qt::blue);
      require(image.save(QString::fromStdString(frame.images[camera].string()), "PNG"), "Cannot seed recovery PNG");
    }
    empty.frames.push_back(frame);
  }
  document["hstream_ui"]["stitching_calibration"]["match_snapshot"] =
      take(hm::stitching::PublishCalibrationMatches(game, empty));
  config(game, document);
}
} // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  if (argc == 3 && std::string(argv[1]) == "--seed") {
    try {
      seed_empty_matches(argv[2]);
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  if (argc != 2)
    return 1;
  try {
    const auto binary = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    QTemporaryDir temporary;
    require(temporary.isValid(), "Cannot create match fixture");
    const fs::path root(temporary.path().toStdString()), game = root / "game";
    fs::create_directory(game);
    auto document = YAML::Clone(take(hm::baseline_config::load()).values);
    std::ofstream(game / "left.mp4") << "left";
    std::ofstream(game / "right.mp4") << "right";
    document["game"]["videos"]["left"].push_back("left.mp4");
    document["game"]["videos"]["right"].push_back("right.mp4");
    document["game"]["stitching"]["frame_offsets"]["left"] = 0;
    document["game"]["stitching"]["frame_offsets"]["right"] = 0;
    document["stitching"]["calibration_frame_count"] = 2;
    auto owner = document["hstream_ui"]["stitching_calibration"];
    owner["invalidation_id"] = "matches-owner";
    owner["status"] = "complete";
    owner["frame_count"] = 2;
    owner["control_points"] = 100;
    hm::stitching::CalibrationMatchSet automatic;
    automatic.matcher = hm::stitching::ControlPointMatcher::kAkazeHamming;
    automatic.source_context = take(hm::stitching::CalibrationMatchSourceContext(document, game));
    for (int pair = 0; pair < 2; ++pair) {
      hm::stitching::CalibrationMatchFrame frame;
      for (int camera = 0; camera < 2; ++camera) {
        frame.images[camera] = game / (std::to_string(pair) + (camera ? "right.png" : "left.png"));
        frame.sizes[camera] = {80, 60};
        frame.source_paths[camera] = (game / (camera ? "right.mp4" : "left.mp4")).string();
        frame.source_seconds[camera] = pair + .25;
        QImage image(80, 60, QImage::Format_RGB888);
        image.fill(camera ? Qt::green : Qt::blue);
        require(image.save(QString::fromStdString(frame.images[camera].string()), "PNG"), "Cannot save original PNG");
      }
      if (pair)
        frame.matches.push_back({{10.25f, 20}, {12, 21.5f}, .75f});
      automatic.frames.push_back(frame);
    }
    const auto original = take(hm::stitching::PublishCalibrationMatches(game, automatic));
    owner["match_snapshot"] = original;
    config(game, document);
    auto receipt = invoke(binary, root, "matches-read", {});
    auto result = receipt.value("result").toObject();
    require(result.value("match_editor_version").toInt() == 1, "Missing editor schema");
    require(result.value("automatic_compatible").toBool(), "Automatic reset must be compatible");
    require(result.value("fingerprint").toString().toStdString() == original, "Wrong snapshot identity");
    auto frames = result.value("frames").toArray();
    require(
        frames.size() == 2 && frames[0].toObject().value("matches").toArray().isEmpty(),
        "Empty pairs must be editable");
    require(frames[1].toObject().value("source_seconds").toArray()[0].toDouble() == 1.25, "Lost source time");
    require(frames[0].toObject().value("source_identity").toArray()[0].toString().size() == 64, "Lost source identity");
    require(
        result.value("workspace").toObject().value("game_id").toString() == "game",
        "Main calibration has no edit workspace");
    require(
        result.value("editor_limits").toObject().value("maximum_matches_per_pair").toInt() ==
            hm::stitching::kMaximumEditableMatchesPerPair,
        "Editor limit differs from native");
    const QJsonObject point{
        {"left", QJsonArray{11.5, 20}},
        {"right", QJsonArray{12, 21.5}},
        {"score", .625},
        {"left_index", 7},
        {"right_index", 8}};
    receipt = invoke(
        binary,
        root,
        "matches-publish",
        {{"fingerprint", QString::fromStdString(original)},
         {"frames", QJsonArray{QJsonObject{{"index", 1}, {"matches", QJsonArray{point}}}}}});
    const auto manual = receipt.value("result").toObject().value("fingerprint").toString();
    owner["match_snapshot"] = manual.toStdString();
    config(game, document);
    result = invoke(binary, root, "matches-read", {}).value("result").toObject();
    require(
        result.value("manual").toBool() && result.value("automatic_compatible").toBool(), "Manual reset lost original");
    require(
        result.value("automatic").toObject().value("fingerprint").toString().toStdString() == original,
        "Reset must retain the exact automatic snapshot");
    require(
        result.value("frames")
                .toArray()[1]
                .toObject()
                .value("matches")
                .toArray()[0]
                .toObject()
                .value("left")
                .toArray()[0]
                .toDouble() == 11.5,
        "Edited native coordinates changed");
    auto stale = invoke(
        binary,
        root,
        "experiment-save-matches",
        {{"workspace", result.value("workspace")},
         {"fingerprint", QString::fromStdString(original)},
         {"sequence", 1},
         {"frames", QJsonArray{QJsonObject{{"index", 1}, {"matches", QJsonArray{point}}}}}},
        false);
    require(stale.value("error").toString().contains("changed while the editor"), "Stale edit was not fenced");
    const auto saved = take(hm::stitching::LoadCalibrationMatches(game, manual.toStdString()));
    std::ofstream(saved.frames[0].images[0], std::ios::app) << "tampered";
    receipt = invoke(binary, root, "matches-read", {}, false);
    require(receipt.value("error").toString().contains("image changed"), "Tampered original image was not rejected");

    // A controlled extractor publishes bounded original images then fails its
    // automatic solve, exactly the recoverable insufficient-matches boundary.
    // This is a process/receipt test and performs no GPU work.
    const auto recovery = root / "recovery";
    fs::create_directories(recovery / "game");
    fs::copy_file(game / "left.mp4", recovery / "game/left.mp4");
    fs::copy_file(game / "right.mp4", recovery / "game/right.mp4");
    owner.remove("match_snapshot");
    owner["status"] = "pending";
    config(recovery / "game", document);
    const auto exporter = recovery / "exporter";
    std::ofstream(exporter)
        << "#!/bin/sh\nwhile [ $# -gt 0 ]; do\n"
           "case \"$1\" in --output) output=\"$2\"; shift;; --game-dir) game=\"$2\"; shift;; esac\nshift\ndone\n"
           "printf '#!/bin/sh\\n\"%s\" --seed \"%s\"\\nexit 1\\n' '"
        << QFileInfo(QString::fromLocal8Bit(argv[0])).absoluteFilePath().toStdString() << "' \"$game\" > \"$output\"\n";
    fs::permissions(exporter, fs::perms::owner_all);
    const QJsonObject runtime{
        {"root", QString::fromStdString(recovery.string())},
        {"exporter", QString::fromStdString(exporter.string())},
        {"cli", QString::fromStdString(exporter.string())},
        {"config", QString::fromStdString((recovery / "game/config.yaml").string())}};
    result =
        invoke(binary, recovery, "experiment-recover", {{"settings", QJsonObject{}}, {"sequence", 1}}, true, runtime)
            .value("result")
            .toObject();
    require(
        result.value("state").toString() == "editable" && !result.value("calibration_complete").toBool(),
        "Recovery must never advertise a failed automatic solve as calibration success");
    require(
        result.value("frames").toArray().size() == 2 &&
            result.value("frames").toArray()[0].toObject().value("matches").toArray().isEmpty(),
        "Zero-match recovery must retain editable original frames");
    const auto workspace = result.value("workspace").toObject();
    receipt = invoke(binary, recovery, "experiment-promote", {{"workspace", workspace}}, false);
    require(
        receipt.value("state").toString() == "failed" && receipt.value("artifacts").toArray().isEmpty(),
        "Unsolved recovery was promoted");
    require(
        YAML::LoadFile((recovery / workspace.value("game_directory").toString().toStdString() / "config.yaml")
                           .string())["hstream_ui"]["stitching_calibration"]["status"]
                .as<std::string>() == "pending",
        "Recovery changed the pending calibration into success");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
