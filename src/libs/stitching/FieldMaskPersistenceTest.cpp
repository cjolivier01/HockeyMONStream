#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/RinkSegmentation.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>

#include <opencv2/imgcodecs.hpp>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}
} // namespace

int main() {
  bool ok = true;
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("field-mask-persistence-test-" + std::to_string(::getpid()));
  fs::create_directories(root);
  {
    std::ofstream config(root / "config.yaml");
    config << "unrelated:\n  keep: true\n";
  }
  for (const char* name : {"hm_project.pto", "autooptimiser_out.pto"})
    std::ofstream(root / name) << "p f2 w32 h24\n";
  for (const char* name : {
           "mapping_0000.tif",
           "mapping_0000_x.tif",
           "mapping_0000_y.tif",
           "mapping_0001.tif",
           "mapping_0001_x.tif",
           "mapping_0001_y.tif",
       }) {
    cv::imwrite((root / name).string(), cv::Mat(24, 32, CV_32F, cv::Scalar(0.0f)));
  }
  auto initial_hugin_lock = hm::stitching::HuginProject::RecoverAndLock(root);
  ok &= expect(initial_hugin_lock.ok(), "generation test must lock initial Hugin artifacts");
  std::string initial_output_generation;
  if (initial_hugin_lock.ok()) {
    auto initial_hugin_generation = hm::stitching::HuginProject::GenerationId(root, **initial_hugin_lock);
    ok &= expect(initial_hugin_generation.ok(), "generation test must identify initial Hugin artifacts");
    if (initial_hugin_generation.ok()) {
      auto generation = hm::stitching::stitched_output_generation_id(*initial_hugin_generation, 0.0);
      ok &= expect(generation.ok(), "generation test must identify initial stitched output");
      if (generation.ok())
        initial_output_generation = *generation;
    }
    initial_hugin_lock->reset();
  }
  cv::Mat first(24, 32, CV_8U, cv::Scalar(0));
  cv::Mat second(24, 32, CV_8U, cv::Scalar(0));
  first(cv::Rect(2, 3, 10, 8)).setTo(255);
  second(cv::Rect(20, 10, 8, 10)).setTo(255);
  hm::stitching::RinkProfile profile;
  profile.masks = {first, second};
  profile.centroid = {15.25, 11.5};
  profile.combined_bbox = {2.0, 3.0, 26.0, 17.0};
  auto status = hm::stitching::save_rink_profile(root.string(), profile);
  if (!status.ok())
    std::cerr << "FAIL: initial rink profile publication: " << status << '\n';
  ok &= expect(status.ok(), "valid rink profile must persist");
  if (status.ok()) {
    ok &=
        expect(!cv::imread((root / "rink_mask_0.png").string(), cv::IMREAD_GRAYSCALE).empty(), "first mask must load");
    ok &=
        expect(!cv::imread((root / "rink_mask_1.png").string(), cv::IMREAD_GRAYSCALE).empty(), "second mask must load");
    struct stat config_metadata{};
    ok &= expect(
        ::stat((root / "config.yaml").c_str(), &config_metadata) == 0 && (config_metadata.st_mode & 0777) == 0600,
        "rink profile publication must keep the private config owner-only");
    const YAML::Node config = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(config["unrelated"]["keep"].as<bool>(), "unrelated config must survive");
    ok &= expect(config["rink"]["ice_contours_mask_count"].as<int>() == 2, "mask count must match files");
    ok &= expect(
        config["rink"]["stitched_output_generation"].as<std::string>() == initial_output_generation,
        "rink profile must persist the exact stitched-output generation");
    const YAML::Node bbox = config["rink"]["ice_contours_combined_bbox"];
    ok &= expect(bbox[0].as<double>() == 2.0 && bbox[2].as<double>() == 28.0, "bbox must persist as x1,y1,x2,y2");

    YAML::Node guarded_config = YAML::LoadFile((root / "config.yaml").string());
    YAML::Node guarded_calibration = guarded_config["hstream_ui"]["stitching_calibration"];
    guarded_calibration["status"] = "pending";
    guarded_calibration["artifacts_invalidated"] = true;
    guarded_calibration["invalidation_id"] = "rink-run-a";
    {
      std::ofstream output(root / "config.yaml");
      output << guarded_config << '\n';
    }
    ok &= expect(
        hm::stitching::save_rink_profile(root.string(), profile, "rink-run-a").ok(),
        "current calibration token must permit transactional rink publication");
    YAML::Node completed_config = YAML::LoadFile((root / "config.yaml").string());
    YAML::Node completed_calibration = completed_config["hstream_ui"]["stitching_calibration"];
    ok &= expect(
        completed_calibration["status"].as<std::string>("") == "complete" && !completed_calibration["stale_from"] &&
            !completed_calibration["artifacts_invalidated"] &&
            completed_calibration["invalidation_id"].as<std::string>("") == "rink-run-a",
        "final rink publication must complete the active calibration generation atomically");
    completed_calibration["status"] = "complete";
    completed_calibration.remove("artifacts_invalidated");
    completed_config["stitching"]["post_stitch_rotate_degrees"] = 2.5;
    {
      std::ofstream output(root / "config.yaml");
      output << completed_config << '\n';
    }
    ok &= expect(
        hm::stitching::save_rink_profile(root.string(), profile, "rink-run-a").ok() &&
            hm::stitching::is_field_mask_configured(root.string()),
        "the completed generation owner must be able to publish a live-rotation rink generation");
    YAML::Node superseding_config = YAML::LoadFile((root / "config.yaml").string());
    superseding_config["hstream_ui"]["stitching_calibration"]["status"] = "pending";
    superseding_config["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = false;
    superseding_config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "rink-run-b";
    {
      std::ofstream output(root / "config.yaml");
      output << superseding_config << '\n';
    }
    const auto superseded_rink = hm::stitching::save_rink_profile(root.string(), profile, "rink-run-a");
    const YAML::Node after_superseded_rink = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(
        absl::IsAborted(superseded_rink) &&
            after_superseded_rink["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>() ==
                "rink-run-b",
        "superseded rink publication must preserve the newer invalidation generation");

    const cv::Mat committed_snapshot(24, 32, CV_8UC3, cv::Scalar(1, 2, 3));
    ok &= expect(
        cv::imwrite((root / "s.png").string(), committed_snapshot),
        "superseded stitched snapshot test must publish its committed fixture");
    YAML::Node current_snapshot_owner = YAML::LoadFile((root / "config.yaml").string());
    current_snapshot_owner["hstream_ui"]["stitching_calibration"]["status"] = "complete";
    current_snapshot_owner["hstream_ui"]["stitching_calibration"].remove("stale_from");
    current_snapshot_owner["hstream_ui"]["stitching_calibration"].remove("artifacts_invalidated");
    current_snapshot_owner["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "rink-run-a";
    {
      std::ofstream output(root / "config.yaml");
      output << current_snapshot_owner << '\n';
    }
    const cv::Mat stale_snapshot(24, 32, CV_8UC3, cv::Scalar(200, 100, 50));
    std::atomic<bool> stale_inference_started{false};
    absl::Status stale_snapshot_status = absl::UnknownError("stale rink inference did not run");
    ::setenv("HM_TEST_RINK_PRE_PUBLICATION_DELAY_MS", "300", 1);
    std::thread stale_inference([&] {
      stale_inference_started = true;
      stale_snapshot_status =
          hm::stitching::save_rink_profile_with_stitched_image(root.string(), profile, stale_snapshot, "rink-run-a");
    });
    while (!stale_inference_started)
      std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    {
      auto config_transaction = hm::stitching::GameConfigTransactionLock::Acquire(root);
      ok &= expect(config_transaction.ok(), "newer rink owner must acquire the config transaction lock");
      if (config_transaction.ok()) {
        YAML::Node newer_snapshot_owner = YAML::LoadFile((root / "config.yaml").string());
        newer_snapshot_owner["hstream_ui"]["stitching_calibration"]["status"] = "pending";
        newer_snapshot_owner["hstream_ui"]["stitching_calibration"]["stale_from"] = "input";
        newer_snapshot_owner["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = false;
        newer_snapshot_owner["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "rink-run-b";
        ok &= expect(
            hm::stitching::publish_game_config(root, YAML::Dump(newer_snapshot_owner) + "\n").ok(),
            "newer rink owner must supersede the inference before publication");
      }
    }
    stale_inference.join();
    ::unsetenv("HM_TEST_RINK_PRE_PUBLICATION_DELAY_MS");
    const cv::Mat after_stale_snapshot = cv::imread((root / "s.png").string(), cv::IMREAD_COLOR);
    const YAML::Node after_stale_snapshot_config = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(
        absl::IsAborted(stale_snapshot_status) && !after_stale_snapshot.empty() &&
            cv::norm(after_stale_snapshot, committed_snapshot, cv::NORM_INF) == 0 &&
            after_stale_snapshot_config["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>() ==
                "rink-run-b",
        "superseded rink inference must not overwrite the committed stitched calibration snapshot");

    ::setenv("HM_TEST_RINK_INTERRUPT_AFTER_PREPARE_SYNC", "1", 1);
    const auto interrupted_before_publication = hm::stitching::save_rink_profile(root.string(), profile);
    ::unsetenv("HM_TEST_RINK_INTERRUPT_AFTER_PREPARE_SYNC");
    ok &= expect(
        !interrupted_before_publication.ok(), "injected interruption after durable preparation must stop publication");
    bool durable_prepared_journal = false;
    for (const auto& entry : fs::directory_iterator(root)) {
      if (entry.is_directory() && entry.path().filename().string().rfind(".hstream-rink-", 0) == 0)
        durable_prepared_journal = true;
    }
    ok &= expect(durable_prepared_journal, "durably prepared rink publication must retain its recovery journal");
    ok &= expect(
        YAML::LoadFile((root / "config.yaml").string())["unrelated"]["keep"].as<bool>(),
        "interruption after durable preparation must happen before replacing root config");
    ok &= expect(
        hm::stitching::is_field_mask_configured(root.string()),
        "durably prepared rink publication must recover on the next owner");

    const fs::path replacement_mapping = root / "mapping_0000_x.replacement.tif";
    cv::imwrite(replacement_mapping.string(), cv::Mat(24, 32, CV_32F, cv::Scalar(1.0f)));
    fs::rename(replacement_mapping, root / "mapping_0000_x.tif");
    ok &= expect(
        !hm::stitching::is_field_mask_configured(root.string()),
        "same-canvas Hugin recalibration must invalidate a mask from the previous output generation");
    NvBufSurfaceParams stale_surface_params{};
    hm::surface::Surface stale_surface(&stale_surface_params);
    const auto stale_publication =
        hm::stitching::create_field_mask(root.string(), stale_surface, initial_output_generation);
    ok &= expect(
        absl::IsAborted(stale_publication),
        "downstream inference must reject a frame produced by the previous Hugin generation");
    ok &= expect(
        hm::stitching::save_rink_profile(root.string(), profile).ok() &&
            hm::stitching::is_field_mask_configured(root.string()),
        "regenerating the profile must bind it to the recalibrated same-size output");
    YAML::Node rotated_config = YAML::LoadFile((root / "config.yaml").string());
    rotated_config["stitching"]["post_stitch_rotate_degrees"] = 5.0;
    {
      std::ofstream output(root / "config.yaml");
      output << rotated_config << '\n';
    }
    ok &= expect(
        !hm::stitching::is_field_mask_configured(root.string()),
        "a post-stitch rotation change must invalidate a mask even when canvas dimensions do not change");
    ok &= expect(
        hm::stitching::save_rink_profile(root.string(), profile).ok() &&
            hm::stitching::is_field_mask_configured(root.string()),
        "regenerating the profile must bind it to the rotated output generation");

    auto runtime_hugin_lock = hm::stitching::HuginProject::RecoverAndLock(root);
    ok &= expect(runtime_hugin_lock.ok(), "runtime-override test must lock Hugin artifacts");
    std::string runtime_override_generation;
    if (runtime_hugin_lock.ok()) {
      auto runtime_hugin_generation = hm::stitching::HuginProject::GenerationId(root, **runtime_hugin_lock);
      ok &= expect(runtime_hugin_generation.ok(), "runtime-override test must identify Hugin artifacts");
      if (runtime_hugin_generation.ok()) {
        auto generation = hm::stitching::stitched_output_generation_id(*runtime_hugin_generation, 5.123456789012345);
        ok &= expect(generation.ok(), "runtime-override test must identify the exact rotated output");
        if (generation.ok())
          runtime_override_generation = *generation;
      }
      runtime_hugin_lock->reset();
    }
    YAML::Node runtime_override_config = YAML::LoadFile((root / "config.yaml").string());
    runtime_override_config["stitching"]["post_stitch_rotate_degrees"] = 0.0;
    runtime_override_config["rink"]["stitched_output_generation"] = runtime_override_generation;
    {
      std::ofstream output(root / "config.yaml");
      output << runtime_override_config << '\n';
    }
    ok &= expect(
        !runtime_override_generation.empty() &&
            hm::stitching::is_field_mask_configured(root.string(), runtime_override_generation),
        "runtime output generation must be authoritative when its Hugin component is current");
    ok &= expect(
        !hm::stitching::is_field_mask_configured(root.string()),
        "persisted rotation must not accidentally validate a different runtime output generation");
    ok &= expect(
        !hm::stitching::is_field_mask_configured(root.string(), initial_output_generation),
        "runtime generation validation must reject a stale Hugin component independently of rotation");
    ok &= expect(
        hm::stitching::save_rink_profile(root.string(), profile).ok() &&
            hm::stitching::is_field_mask_configured(root.string()),
        "runtime-override validation must leave a regenerable persisted configuration");

    // Simulate SIGKILL after a prepared transaction published only part of a
    // new generation. The next field-mask read must restore the complete old
    // generation before consuming any artifact.
    const fs::path interrupted = root / ".hstream-rink-interrupted";
    fs::create_directories(interrupted / "previous");
    fs::copy_file(root / "config.yaml", interrupted / "previous" / "config.yaml");
    fs::copy_file(root / "rink_mask_0.png", interrupted / "previous" / "rink_mask_0.png");
    fs::copy_file(root / "rink_mask_1.png", interrupted / "previous" / "rink_mask_1.png");
    {
      std::ofstream(interrupted / "new-files") << "rink_mask_0.png\nrink_mask_1.png\nconfig.yaml\n";
      std::ofstream(interrupted / "state") << "PREPARED\n";
      std::ofstream(root / "config.yaml") << "interrupted: true\n";
      cv::imwrite((root / "rink_mask_0.png").string(), cv::Mat(2, 2, CV_8U, cv::Scalar(255)));
    }
    ::setenv("HM_TEST_RINK_ROLLBACK_FAIL_AFTER", "1", 1);
    ok &= expect(
        !hm::stitching::is_field_mask_configured(root.string()),
        "an interrupted rink rollback must remain recoverable");
    ::unsetenv("HM_TEST_RINK_ROLLBACK_FAIL_AFTER");
    ok &= expect(fs::exists(interrupted), "an interrupted rink rollback must retain its transaction");
    ok &= expect(
        fs::is_regular_file(interrupted / "previous" / "config.yaml") &&
            fs::is_regular_file(interrupted / "previous" / "rink_mask_0.png") &&
            fs::is_regular_file(interrupted / "previous" / "rink_mask_1.png"),
        "an interrupted rink rollback must retain every durable backup");
    ok &= expect(hm::stitching::is_field_mask_configured(root.string()), "prepared rink transaction must recover");
    const YAML::Node recovered = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(recovered["unrelated"]["keep"].as<bool>(), "rink recovery must restore the prior config");
    ok &= expect(
        cv::imread((root / "rink_mask_0.png").string(), cv::IMREAD_GRAYSCALE).size() == cv::Size(32, 24),
        "rink recovery must restore the prior mask generation");
    ok &= expect(!fs::exists(interrupted), "recovered rink transaction must be cleaned");

    const fs::path malformed = root / ".hstream-rink-malformed";
    fs::create_directories(malformed);
    std::ofstream(malformed / "state") << "PREPARE\n";
    ok &= expect(
        !hm::stitching::is_field_mask_configured(root.string()), "unknown rink transaction state must fail closed");
    ok &= expect(fs::exists(malformed), "unknown rink transaction state must preserve its journal");
    ok &= expect(
        YAML::LoadFile((root / "config.yaml").string())["unrelated"]["keep"].as<bool>(),
        "unknown rink transaction state must not touch the committed profile");
    fs::remove_all(malformed);

    const fs::path multiline = root / ".hstream-rink-multiline";
    fs::create_directories(multiline);
    std::ofstream(multiline / "state") << "PREPARED\n\nCOMMITTED\n";
    ok &= expect(
        !hm::stitching::is_field_mask_configured(root.string()), "multiline rink transaction state must fail closed");
    ok &= expect(fs::exists(multiline), "multiline rink transaction state must preserve its journal");
    ok &= expect(fs::is_regular_file(root / "config.yaml"), "multiline rink state must not touch the profile");
    fs::remove_all(multiline);

    const fs::path nonregular = root / ".hstream-rink-nonregular";
    fs::create_directories(nonregular / "state");
    ok &= expect(
        !hm::stitching::is_field_mask_configured(root.string()), "non-regular rink transaction state must fail closed");
    ok &= expect(fs::exists(nonregular), "non-regular rink transaction state must preserve its journal");
    fs::remove_all(nonregular);

    const fs::path missing_manifest = root / ".hstream-rink-missing-manifest";
    fs::create_directories(missing_manifest / "previous");
    std::ofstream(missing_manifest / "state") << "PREPARED\n";
    ok &= expect(
        !hm::stitching::is_field_mask_configured(root.string()),
        "prepared rink transaction without a manifest must fail closed");
    ok &= expect(fs::exists(missing_manifest), "missing rink manifest must preserve its journal");
    fs::remove_all(missing_manifest);

    const fs::path malicious = root / ".hstream-rink-malicious";
    fs::create_directories(malicious / "previous");
    std::ofstream(malicious / "state") << "PREPARED\n";
    std::ofstream(malicious / "new-files") << "rink_mask_0.png\nconfig.yaml\n.hstream-rink.lock\n";
    ok &= expect(
        !hm::stitching::is_field_mask_configured(root.string()), "unexpected rink manifest artifact must fail closed");
    ok &= expect(fs::exists(malicious), "invalid rink manifest must preserve its journal");
    ok &= expect(fs::is_regular_file(root / "config.yaml"), "invalid rink manifest must not remove profile files");
    fs::remove_all(malicious);

    const fs::path unprepared = root / ".hstream-rink-unprepared";
    fs::create_directories(unprepared);
    std::ofstream(unprepared / "temporary") << "not published\n";
    ok &= expect(
        hm::stitching::is_field_mask_configured(root.string()) && !fs::exists(unprepared),
        "unprepared rink staging without publication metadata must be cleaned");

    const fs::path committed = root / ".hstream-rink-committed";
    fs::create_directories(committed);
    std::ofstream(committed / "state") << "COMMITTED\n";
    ok &= expect(
        hm::stitching::is_field_mask_configured(root.string()) && !fs::exists(committed),
        "committed rink journal must be cleaned without rollback");

    hm::stitching::RinkProfile replacement_profile = profile;
    replacement_profile.masks[0] = cv::Mat(24, 32, CV_8U, cv::Scalar(255));
    std::atomic<bool> atomic_read_finished{false};
    std::atomic<bool> concurrent_publication_finished{false};
    absl::StatusOr<cv::Mat> atomic_read = absl::UnknownError("field-mask reader did not run");
    absl::Status concurrent_publication = absl::UnknownError("field-mask writer did not run");
    ::setenv("HM_TEST_FIELD_MASK_PRE_DECODE_DELAY_MS", "150", 1);
    std::thread atomic_reader([&] {
      atomic_read = hm::stitching::load_field_mask(root.string());
      atomic_read_finished = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::thread concurrent_publisher([&] {
      concurrent_publication = hm::stitching::save_rink_profile(root.string(), replacement_profile);
      concurrent_publication_finished = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ok &= expect(!atomic_read_finished, "field-mask decode delay must hold the generation read open");
    ok &= expect(
        !concurrent_publication_finished,
        "field-mask publication must wait until validation and decoding release the generation locks");
    atomic_reader.join();
    concurrent_publisher.join();
    ::unsetenv("HM_TEST_FIELD_MASK_PRE_DECODE_DELAY_MS");
    ok &= expect(
        atomic_read.ok() && atomic_read->at<uchar>(0, 0) == 0,
        "an atomic field-mask read must decode the generation that it validated");
    ok &= expect(concurrent_publication.ok(), "waiting field-mask publication must resume after the atomic read");
    const auto replacement_read = hm::stitching::load_field_mask(root.string());
    ok &= expect(
        replacement_read.ok() && replacement_read->at<uchar>(0, 0) == 255,
        "the next field-mask read must observe the concurrently published generation");

    cv::Mat color_mask(24, 32, CV_8UC3, cv::Scalar(0, 64, 255));
    ok &= expect(
        cv::imwrite((root / "rink_mask_0.png").string(), color_mask),
        "color field-mask compatibility test must write its fixture");
    const auto grayscale_read = hm::stitching::load_field_mask(root.string());
    ok &= expect(
        grayscale_read.ok() && grayscale_read->type() == CV_8UC1 && grayscale_read->size() == color_mask.size(),
        "field-mask loading must preserve the previous 8-bit grayscale decode contract");
    ok &= expect(
        hm::stitching::save_rink_profile(root.string(), profile).ok(),
        "field-mask compatibility test must restore the committed profile");

    auto hugin_lock = hm::stitching::HuginProject::RecoverAndLock(root);
    ok &= expect(hugin_lock.ok(), "field-mask lock test must acquire the Hugin artifact lock");
    std::atomic<bool> field_mask_read_finished{false};
    std::atomic<bool> field_mask_read_result{false};
    std::atomic<bool> field_mask_write_finished{false};
    std::atomic<bool> field_mask_write_result{false};
    std::thread field_mask_reader([&] {
      field_mask_read_result = hm::stitching::is_field_mask_configured(root.string());
      field_mask_read_finished = true;
    });
    std::thread field_mask_writer([&] {
      field_mask_write_result = hm::stitching::save_rink_profile(root.string(), profile).ok();
      field_mask_write_finished = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ok &= expect(
        !field_mask_read_finished, "field-mask validation must wait for a concurrent Hugin publication to finish");
    ok &= expect(
        !field_mask_write_finished,
        "field-mask publication must wait for a concurrent Hugin generation writer to finish");
    if (hugin_lock.ok())
      hugin_lock->reset();
    field_mask_reader.join();
    field_mask_writer.join();
    ok &= expect(
        field_mask_read_finished && field_mask_read_result,
        "field-mask validation must resume after the Hugin artifact lock is released");
    ok &= expect(
        field_mask_write_finished && field_mask_write_result,
        "field-mask publication must resume after the Hugin artifact lock is released");
  }
  hm::stitching::RinkProfile one_mask = profile;
  one_mask.masks.resize(1);
  status = hm::stitching::save_rink_profile(root.string(), one_mask);
  ok &= expect(status.ok(), "a smaller rink mask generation must persist");
  ok &= expect(!fs::exists(root / "rink_mask_1.png"), "obsolete rink masks must be removed transactionally");
  const auto config_bytes = [&]() {
    std::ifstream input(root / "config.yaml", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  for (int invalid_case = 0; invalid_case < 3; ++invalid_case) {
    hm::stitching::RinkProfile invalid = one_mask;
    if (invalid_case == 0)
      invalid.centroid.x = std::numeric_limits<double>::quiet_NaN();
    else if (invalid_case == 1)
      invalid.combined_bbox.width = std::numeric_limits<double>::infinity();
    else
      invalid.scores = {std::numeric_limits<float>::quiet_NaN()};
    ok &= expect(
        !hm::stitching::save_rink_profile(root.string(), invalid).ok(),
        "non-finite rink geometry or scores must not be persisted");
    std::ifstream input(root / "config.yaml", std::ios::binary);
    const std::string after{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    ok &= expect(after == config_bytes, "invalid rink profiles must preserve the committed config generation");
  }
  profile.masks[1] = cv::Mat(10, 10, CV_8U);
  ok &= expect(!hm::stitching::save_rink_profile(root.string(), profile).ok(), "mixed mask dimensions must fail");
  fs::remove_all(root);
  return ok ? 0 : 1;
}
