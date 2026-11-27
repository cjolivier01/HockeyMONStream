#include "src/apps/hstream-ui/ExperimentTaskRuntime.h"
#include "hstream/src/libs/stitching/PlayerFrameInputStore.h"

#include <opencv2/imgcodecs.hpp>
#include <unistd.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace hm::stitching;
namespace {
void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
void write_bytes(const fs::path& path, const std::string& value) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << value;
  require(bool(output), "Cannot write fixture");
}
std::string read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
PlayerFrameSelectionPlan make_plan(const fs::path& root) {
  PlayerFrameSelectionPlan plan;
  plan.settings.frame_count = 2;
  plan.context = {
      {"source_context", "fixed-camera-chapters-offsets-anchor"},
      {"baseline_generation", "baseline"},
      {"output_generation", "output"},
      {"detector_identity", "detector"},
      {"rink_mask_sha256", "mask-content"},
      {"rink_mask_revision", "output:authority"},
      {"fieldmask_settings", "center=-0.1;bottom=0.1"},
      {"output_rotation_degrees", "0.000000"}};
  for (const char* role : {"left", "right"}) {
    const fs::path video = root / (std::string(role) + ".mp4");
    write_bytes(video, "recorded-source-fixture");
    auto binding = BindPlayerFrameSource(video);
    if (!binding.ok())
      throw std::runtime_error(binding.status().ToString());
    plan.sources.push_back(*binding);
  }
  for (uint64_t index = 0; index < 2; ++index) {
    PlayerFrameObservation observation;
    observation.pair.timeline_pts_ns = (10 + index * 2) * kPlayerFrameSecond;
    for (size_t camera = 0; camera < 2; ++camera) {
      observation.pair.cameras[camera] = {
          plan.sources[camera].path,
          observation.pair.timeline_pts_ns + camera * 123,
          static_cast<uint32_t>(camera),
          300 + index * 60};
    }
    if (index > 0) {
      observation.eligible_people = 1;
      observation.size_band_counts[0] = 1;
      observation.coverage = {7};
      observation.quality = 0.9;
    }
    plan.selected.push_back(observation);
  }
  auto fingerprint = PlayerFrameSelectionFingerprint(plan);
  if (!fingerprint.ok())
    throw std::runtime_error(fingerprint.status().ToString());
  plan.fingerprint = *fingerprint;
  const auto status = ValidatePlayerFrameSelectionPlan(plan);
  if (!status.ok())
    throw std::runtime_error(status.ToString());
  return plan;
}

std::vector<std::array<fs::path, 2>> make_images(const fs::path& root) {
  fs::create_directories(root);
  std::vector<std::array<fs::path, 2>> images(2);
  for (size_t index = 0; index < images.size(); ++index) {
    for (size_t camera = 0; camera < 2; ++camera) {
      images[index][camera] = root / (std::to_string(index) + "-" + std::to_string(camera) + ".png");
      const cv::Mat image = index == 0 ? cv::Mat(48, 1536, CV_8UC3, cv::Scalar(19 + camera, 70, 151))
                                       : cv::Mat(48, 1536, CV_16UC3, cv::Scalar(4096 + camera * 256, 16384, 32768));
      if (!cv::imwrite(images[index][camera].string(), image, {cv::IMWRITE_PNG_COMPRESSION, 0}))
        throw std::runtime_error("Cannot create PNG fixture");
    }
  }
  return images;
}

void run(const fs::path& root) {
  const auto plan = make_plan(root);
  const auto images = make_images(root / "captured");
  StitchingExperimentWorkspace saved;
  saved.root = root;
  saved.game_directory = root / "game";
  saved.game_id = "game";
  saved.invalidation_id = "owned-invalidation";
  saved.settings.frame_count = 2;
  fs::create_directories(saved.game_directory / "_hockeymon");
  require(PublishPlayerFrameInputs(saved.game_directory, plan, images).ok(), "Publish retained inputs");
  const auto config_path = saved.game_directory / "_hockeymon/config.yaml";
  YAML::Node config;
  config["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(plan);
  config["stitching"]["calibration_frame_inputs_fingerprint"] = plan.fingerprint;
  config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = saved.invalidation_id;
  config["hstream_ui"]["stitching_calibration"]["status"] = "complete";
  auto publish = [&] { write_bytes(config_path, YAML::Dump(config)); };
  publish();
  hm::web::Request request;
  request.root = root / "request";
  request.runtime = {
      {"root", QString::fromStdString(root.string())},
      {"cli", "/bin/true"},
      {"exporter", "/bin/true"},
      {"config", QString::fromStdString(config_path.string())}};
  int processes = 0;
  const auto process = [&](const QString&, const QStringList&, const QString&, const QProcessEnvironment&) {
    ++processes;
    throw std::runtime_error("Unexpected processing of retained inputs");
  };
  auto materialize = [&] { hm::experiment::run_experiment(request, saved, process, false, true); };
  auto rejected = [&](const auto& action, const std::string& expected) {
    try {
      action();
    } catch (const std::exception& error) {
      require(std::string(error.what()).find(expected) != std::string::npos, error.what());
      return;
    }
    throw std::runtime_error("Expected rejection: " + expected);
  };
  const auto completed = read_bytes(config_path);
  materialize();
  require(read_bytes(config_path) == completed, "Completed config changed during retained-frame reuse");
  require(!fs::exists(request.root), "Retained-frame reuse created runtime work");
  config["hstream_ui"]["stitching_calibration"]["status"] = "pending";
  publish();
  const auto pending = read_bytes(config_path);
  materialize();
  require(read_bytes(config_path) == pending, "Pending config changed during retained-frame reuse");
  config["hstream_ui"]["stitching_calibration"]["status"] = "complete";
  config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "replacement";
  publish();
  rejected(materialize, "superseded");
  config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = saved.invalidation_id;
  publish();
  rejected([&] { hm::experiment::run_experiment(request, saved, process); }, "superseded");
  saved.settings.frame_count = 3;
  rejected(materialize, "superseded");
  saved.settings.frame_count = 2;
  fs::remove_all(saved.game_directory / "_hockeymon/player-frame-inputs");
  rejected(materialize, "superseded");
  require(processes == 0, "Invalid or completed work launched a process");
}
} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--forward") {
    QProcess job;
    hm::experiment::forward_job_output(job);
    job.start(
        "/bin/sh",
        {"-c", "printf 'HSTREAM_RECORD ok\\n'; printf decoder >&2; sleep .1; printf ' ready\\nwarning\\rpartial' >&2"});
    return job.waitForFinished() ? job.exitCode() : 1;
  }
  if (argc == 2 && std::string(argv[1]) == "--interleave") {
    QProcess job;
    hm::experiment::forward_job_output(job);
    job.start(
        "/bin/sh",
        {"-c",
         "printf HST; sleep .05; printf REAM_GRAPH; head -c 140000 /dev/zero | tr '\\0' x; "
         "printf interrupted >&2; sleep .05; printf ' end\\n'"});
    return job.waitForFinished() ? job.exitCode() : 1;
  }
  if (argc == 2 && std::string(argv[1]) == "--oversized") {
    QProcess job;
    hm::experiment::forward_job_output(job);
    job.start(
        "/bin/sh",
        {"-c",
         "printf HSTREAM_LARGE; head -c 9000000 /dev/zero | tr '\\0' x; "
         "printf '\\nHSTREAM_NEXT ok\\n'"});
    return job.waitForFinished() ? job.exitCode() : 1;
  }
  if (argc == 3 && std::string(argv[1]) == "--hold-prefix") {
    QProcess job;
    hm::experiment::forward_job_output(job);
    job.start(
        "/bin/sh",
        {"-c",
         "printf 'progress\\nHST'; while [ ! -f \"$1\" ]; do sleep .02; done; printf 'REAM_OK\\n'",
         "sh",
         argv[2]});
    return job.waitForFinished() ? job.exitCode() : 1;
  }
  char pattern[] = "/tmp/hstream-retained-frames-XXXXXX";
  const char* directory = mkdtemp(pattern);
  if (!directory)
    return 1;
  const fs::path root(directory);
  try {
    QProcess capture;
    capture.start(argv[0], {"--forward"});
    require(capture.waitForFinished() && capture.exitCode() == 0, "Cannot inspect experiment job output");
    require(capture.readAllStandardOutput() == "HSTREAM_RECORD ok\n", "Native stdout changed");
    require(
        capture.readAllStandardError() == "[stderr] decoder ready\n[stderr] warning\r[stderr] partial",
        "Fragmented or trailing job stderr lost its stream identity");
    capture.setProcessChannelMode(QProcess::MergedChannels);
    capture.start(argv[0], {"--interleave"});
    require(capture.waitForFinished() && capture.exitCode() == 0, "Cannot read interleaved experiment logs");
    const QByteArray merged = capture.readAllStandardOutput();
    const QByteArray graph = "HSTREAM_GRAPH" + QByteArray(140000, 'x') + " end\n";
    require(
        merged == "[stderr] interrupted\n" + graph || merged == graph + "[stderr] interrupted",
        "Delayed stderr split a native stdout protocol record");
    capture.start(argv[0], {"--oversized"});
    require(capture.waitForFinished() && capture.exitCode() == 0, "Cannot read oversize record");
    const auto large = capture.readAllStandardOutput();
    require(
        large.contains("[stderr] WARNING:") && large.contains("[stdout] HSTREAM_LARGE") &&
            !large.contains("\nHSTREAM_LARGE") && large.endsWith("\nHSTREAM_NEXT ok\n") &&
            large.mid(large.indexOf("[stdout] ")).count('x') == 9000000,
        "Oversize record escaped its bound, lost output or swallowed the next protocol line");
    const auto resume = root / "resume";
    capture.start(argv[0], {"--hold-prefix", QString::fromStdString(resume.string())});
    const bool live = capture.waitForReadyRead(5000);
    const auto progress = capture.readAllStandardOutput();
    write_bytes(resume, "");
    require(capture.waitForFinished() && capture.exitCode() == 0, "Cannot finish fragmented prefix fixture");
    require(
        live && progress == "progress\n" && capture.readAllStandardOutput() == "HSTREAM_OK\n",
        "A partial protocol prefix delayed an earlier stdout line");
    run(root);
    fs::remove_all(root);
    std::cout << "Retained selected-frame materialization checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    fs::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
