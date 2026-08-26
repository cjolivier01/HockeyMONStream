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
#include <tiffio.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool write_mapping_tiff(const std::filesystem::path& path, uint32_t width, uint32_t height) {
  TIFF* tif = TIFFOpen(path.c_str(), "w");
  if (!tif)
    return false;
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 32);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, 1.0f);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, 1.0f);
  TIFFSetField(tif, TIFFTAG_XPOSITION, 0.0f);
  TIFFSetField(tif, TIFFTAG_YPOSITION, 0.0f);
  std::vector<float> row(width, 0.0f);
  bool ok = true;
  for (uint32_t y = 0; y < height; ++y) {
    if (TIFFWriteScanline(tif, row.data(), y, 0) < 0) {
      ok = false;
      break;
    }
  }
  TIFFClose(tif);
  return ok;
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
  ok &= expect(
      write_mapping_tiff(root / "mapping_0000.tif", 32, 24) && write_mapping_tiff(root / "mapping_0001.tif", 32, 24),
      "mapping placement TIFFs must be written with spatial metadata");
  for (const char* name : {
           "mapping_0000_x.tif",
           "mapping_0000_y.tif",
           "mapping_0001_x.tif",
           "mapping_0001_y.tif",
       }) {
    cv::imwrite((root / name).string(), cv::Mat(24, 32, CV_32F, cv::Scalar(0.0f)));
  }
  cv::Mat initial_seam(24, 32, CV_8U, cv::Scalar(0));
  initial_seam.colRange(16, initial_seam.cols).setTo(255);
  cv::imwrite((root / "seam_file.png").string(), initial_seam);
  auto initial_hugin_lock = hm::stitching::HuginProject::RecoverAndLock(root);
  ok &= expect(initial_hugin_lock.ok(), "generation test must lock initial Hugin artifacts");
  std::string initial_hugin_generation_id;
  std::string initial_output_generation;
  if (initial_hugin_lock.ok()) {
    auto initial_hugin_generation = hm::stitching::HuginProject::GenerationId(root, **initial_hugin_lock);
    ok &= expect(initial_hugin_generation.ok(), "generation test must identify initial Hugin artifacts");
    if (initial_hugin_generation.ok()) {
      initial_hugin_generation_id = *initial_hugin_generation;
      auto generation = hm::stitching::stitched_output_generation_id(*initial_hugin_generation, 0.0);
      ok &= expect(generation.ok(), "generation test must identify initial stitched output");
      if (generation.ok())
        initial_output_generation = *generation;
    }
    initial_hugin_lock->reset();
  }
  ok &= expect(
      !initial_output_generation.empty() &&
          hm::stitching::validate_stitched_output_generation(root.string(), initial_output_generation).ok(),
      "stitching-only completion must accept the current Hugin generation without a rink mask");
  std::string live_rotation_generation;
  {
    auto hugin_lock = hm::stitching::HuginProject::RecoverAndLock(root);
    ok &= expect(hugin_lock.ok(), "runtime-rotation completion test must lock Hugin artifacts");
    if (hugin_lock.ok()) {
      auto hugin_generation = hm::stitching::HuginProject::GenerationId(root, **hugin_lock);
      ok &= expect(hugin_generation.ok(), "runtime-rotation completion test must identify Hugin artifacts");
      if (hugin_generation.ok()) {
        auto generation = hm::stitching::stitched_output_generation_id(*hugin_generation, 1.0);
        ok &= expect(generation.ok(), "runtime-rotation completion test must identify stitched output");
        if (generation.ok())
          live_rotation_generation = *generation;
      }
    }
  }
  ok &= expect(
      !live_rotation_generation.empty() &&
          hm::stitching::validate_stitched_output_generation(root.string(), live_rotation_generation).ok(),
      "stitching-only completion must accept the live runtime rotation even before Save Preset");
  auto stale_hugin_generation = hm::stitching::stitched_output_generation_id("stale-hugin", 0.0);
  ok &= expect(
      stale_hugin_generation.ok() &&
          absl::IsAborted(hm::stitching::validate_stitched_output_generation(root.string(), *stale_hugin_generation)),
      "stitching-only completion must reject a result from stale Hugin artifacts");
  {
    YAML::Node owned = YAML::LoadFile((root / "config.yaml").string());
    owned["hstream_ui"]["stitching_calibration"]["status"] = "pending";
    owned["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "current-owner";
    std::ofstream(root / "config.yaml") << owned << '\n';
  }
  ok &= expect(
      hm::stitching::validate_stitched_output_generation(root.string(), initial_output_generation, "current-owner")
              .ok() &&
          absl::IsAborted(
              hm::stitching::validate_stitched_output_generation(
                  root.string(), initial_output_generation, "superseded-owner")),
      "stitching-only completion must reject a superseded persisted calibration owner");
  NvBufSurfaceParams cancelled_surface_params{};
  hm::surface::Surface cancelled_surface(&cancelled_surface_params);
  const auto cancelled_rink =
      hm::stitching::create_field_mask(root.string(), cancelled_surface, {}, {}, [] { return true; });
  ok &= expect(
      absl::IsCancelled(cancelled_rink),
      "an already-cancelled rink-mask generation must stop before GPU readback or inference");
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

    auto native_dimensioned_generation =
        hm::stitching::stitched_output_generation_id(initial_hugin_generation_id, 0.0, 32, 24);
    ok &= expect(
        native_dimensioned_generation.ok() &&
            hm::stitching::is_field_mask_configured_for_stitching_config(
                root.string(), /*max_output_width=*/0, /*post_stitch_rotate_degrees=*/0.0) &&
            hm::stitching::is_field_mask_configured(root.string(), *native_dimensioned_generation),
        "startup and dimensioned runtime checks must both accept a size-validated legacy native field mask");
    const YAML::Node migrated_native_config = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(
        native_dimensioned_generation.ok() &&
            migrated_native_config["rink"]["stitched_output_generation"].as<std::string>() ==
                *native_dimensioned_generation,
        "accepting a native legacy field mask must migrate its generation to the dimensioned alias");

    if (native_dimensioned_generation.ok()) {
      const auto replace_legacy_mask = [&](bool corrupt_existing_mask) {
        YAML::Node legacy_config = YAML::LoadFile((root / "config.yaml").string());
        legacy_config["rink"]["stitched_output_generation"] = initial_output_generation;
        legacy_config["rink"]["scoreboard"]["perspective_polygon"] =
            std::vector<std::vector<int>>{{1, 2}, {3, 4}, {5, 6}, {7, 8}};
        {
          std::ofstream output(root / "config.yaml");
          output << legacy_config << '\n';
        }
        if (corrupt_existing_mask) {
          std::ofstream(root / "rink_mask_0.png", std::ios::binary | std::ios::trunc) << "corrupt-mask";
        } else {
          fs::remove(root / "rink_mask_0.png");
        }
        const cv::Mat snapshot(24, 32, CV_8UC3, cv::Scalar(1, 2, 3));
        const absl::Status replacement = hm::stitching::save_rink_profile_with_stitched_image(
            root.string(), profile, snapshot, {}, *native_dimensioned_generation);
        const YAML::Node replaced_config = YAML::LoadFile((root / "config.yaml").string());
        return replacement.ok() &&
            replaced_config["rink"]["stitched_output_generation"].as<std::string>() == *native_dimensioned_generation &&
            replaced_config["rink"]["scoreboard"]["perspective_polygon"].IsSequence() &&
            !cv::imread((root / "rink_mask_0.png").string(), cv::IMREAD_GRAYSCALE).empty();
      };
      ok &= expect(
          replace_legacy_mask(/*corrupt_existing_mask=*/false),
          "a missing native legacy mask must be replaceable without invalidating same-geometry scoreboard data");
      ok &= expect(
          replace_legacy_mask(/*corrupt_existing_mask=*/true),
          "a corrupt native legacy mask must be replaceable without invalidating same-geometry scoreboard data");
    }

    auto scaled_canvas = hm::stitching::stitching_canvas_size(root.string(), /*max_output_width=*/16);
    ok &= expect(scaled_canvas.ok(), "scaled field-mask test must identify output dimensions");
    auto scaled_generation = scaled_canvas.ok()
        ? hm::stitching::stitched_output_generation_id(
              initial_hugin_generation_id, 0.0, scaled_canvas->width, scaled_canvas->height)
        : absl::InvalidArgumentError("scaled canvas unavailable");
    ok &= expect(scaled_generation.ok(), "scaled field-mask test must identify output dimensions");
    ok &= expect(
        !hm::stitching::is_field_mask_configured_for_stitching_config(
            root.string(), /*max_output_width=*/16, /*post_stitch_rotate_degrees=*/0.0),
        "capped field-mask preflight must not accept legacy native-sized masks");
    if (scaled_generation.ok()) {
      YAML::Node scaled_config = YAML::LoadFile((root / "config.yaml").string());
      scaled_config["rink"]["stitched_output_generation"] = *scaled_generation;
      {
        std::ofstream output(root / "config.yaml");
        output << scaled_config << '\n';
      }
      cv::imwrite(
          (root / "rink_mask_0.png").string(),
          cv::Mat(
              static_cast<int>(scaled_canvas->height), static_cast<int>(scaled_canvas->width), CV_8U, cv::Scalar(255)));
      ok &= expect(
          hm::stitching::is_field_mask_configured(root.string(), *scaled_generation) &&
              hm::stitching::is_field_mask_configured_for_stitching_config(
                  root.string(), /*max_output_width=*/16, /*post_stitch_rotate_degrees=*/0.0) &&
              !hm::stitching::is_field_mask_configured(root.string(), initial_output_generation),
          "dimensioned runtime generation must validate field masks against the scaled live canvas");
      cv::imwrite((root / "rink_mask_0.png").string(), first);
      YAML::Node restored_config = YAML::LoadFile((root / "config.yaml").string());
      restored_config["rink"]["stitched_output_generation"] = initial_output_generation;
      {
        std::ofstream output(root / "config.yaml");
        output << restored_config << '\n';
      }
    }

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
            completed_calibration["rink_mask_status"].as<std::string>("") == "complete" &&
            completed_calibration["invalidation_id"].as<std::string>("") == "rink-run-a",
        "final rink publication must complete the active calibration and rink-mask generation atomically");
    completed_calibration["status"] = "complete";
    completed_calibration.remove("artifacts_invalidated");
    completed_config["stitching"]["post_stitch_rotate_degrees"] = 2.5;
    {
      std::ofstream output(root / "config.yaml");
      output << completed_config << '\n';
    }
    ok &= expect(
        hm::stitching::save_rink_profile(root.string(), profile, "rink-run-a").ok() &&
            hm::stitching::is_field_mask_configured(root.string(), {}, "rink-run-a"),
        "the completed generation owner must be able to publish and validate a live-rotation rink generation");
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
                "rink-run-b" &&
            !hm::stitching::is_field_mask_configured(root.string(), {}, "rink-run-a"),
        "superseded rink publication and completion must preserve the newer invalidation generation");

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

    YAML::Node rotation_owner = YAML::LoadFile((root / "config.yaml").string());
    YAML::Node rotation_calibration = rotation_owner["hstream_ui"]["stitching_calibration"];
    rotation_calibration["status"] = "complete";
    rotation_calibration["invalidation_id"] = "rink-run-a";
    rotation_calibration.remove("stale_from");
    rotation_calibration.remove("artifacts_invalidated");
    rotation_owner["stitching"]["post_stitch_rotate_degrees"] = 0.0;
    {
      std::ofstream output(root / "config.yaml");
      output << rotation_owner << '\n';
    }
    const auto pre_rotation_generation =
        hm::stitching::configured_stitched_output_generation_id(root.string(), /*max_output_width=*/0);
    ok &= expect(pre_rotation_generation.ok(), "stale-rotation fixture must have a current output generation");
    if (pre_rotation_generation.ok()) {
      YAML::Node completed_rotation = YAML::LoadFile((root / "config.yaml").string());
      completed_rotation["rink"]["stitched_output_generation"] = *pre_rotation_generation;
      completed_rotation["rink"]["stitched_output_persisted_rotation_degrees"] = 0.0;
      completed_rotation["rink"]["scoreboard"]["perspective_polygon"] =
          std::vector<std::vector<int>>{{1, 2}, {3, 4}, {5, 6}, {7, 8}};
      std::ofstream(root / "config.yaml") << completed_rotation << '\n';
    }
    absl::Status stale_rotation_status = absl::UnknownError("stale rotation publication did not run");
    ::setenv("HM_TEST_RINK_PRE_PUBLICATION_DELAY_MS", "300", 1);
    std::thread stale_rotation_publisher([&] {
      stale_rotation_status = hm::stitching::save_rink_profile_with_stitched_image(
          root.string(),
          profile,
          stale_snapshot,
          "rink-run-a",
          pre_rotation_generation.ok() ? *pre_rotation_generation : std::string());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    {
      auto config_transaction = hm::stitching::GameConfigTransactionLock::Acquire(root);
      ok &= expect(config_transaction.ok(), "rotation writer must acquire the config transaction lock");
      if (config_transaction.ok()) {
        YAML::Node rotated = YAML::LoadFile((root / "config.yaml").string());
        rotated["stitching"]["post_stitch_rotate_degrees"] = 7.5;
        ok &= expect(
            hm::stitching::publish_game_config(root, YAML::Dump(rotated) + "\n").ok(),
            "rotation writer must publish before stale rink inference");
      }
    }
    stale_rotation_publisher.join();
    ::unsetenv("HM_TEST_RINK_PRE_PUBLICATION_DELAY_MS");
    const cv::Mat after_stale_rotation = cv::imread((root / "s.png").string(), cv::IMREAD_COLOR);
    const YAML::Node after_stale_rotation_config = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(
        absl::IsAborted(stale_rotation_status) && !after_stale_rotation.empty() &&
            cv::norm(after_stale_rotation, committed_snapshot, cv::NORM_INF) == 0 &&
            after_stale_rotation_config["stitching"]["post_stitch_rotate_degrees"].as<double>() == 7.5,
        "stale rink inference must not overwrite a newer stitched-output rotation generation");

    std::string live_override_generation;
    std::string newer_live_override_generation;
    auto live_override_canvas_size = hm::stitching::stitching_canvas_size(root.string());
    {
      auto generation_lock = hm::stitching::HuginProject::RecoverAndLock(root);
      ok &= expect(generation_lock.ok(), "runtime-rotation override fixture must lock Hugin artifacts");
      if (generation_lock.ok()) {
        auto hugin_generation = hm::stitching::HuginProject::GenerationId(root, **generation_lock);
        if (hugin_generation.ok() && live_override_canvas_size.ok()) {
          auto generation = hm::stitching::stitched_output_generation_id(
              *hugin_generation, 9.0, live_override_canvas_size->width, live_override_canvas_size->height);
          if (generation.ok())
            live_override_generation = *generation;
          auto newer_generation = hm::stitching::stitched_output_generation_id(
              *hugin_generation, 10.0, live_override_canvas_size->width, live_override_canvas_size->height);
          if (newer_generation.ok())
            newer_live_override_generation = *newer_generation;
        }
      }
    }
    const auto live_override_authorization =
        hm::stitching::authorize_live_stitched_output_rotation(root.string(), 9.0, "field-mask-live-9-a");
    const std::string live_override_authorization_id =
        live_override_authorization.ok() ? live_override_authorization->authorization_id : std::string();
    const YAML::Node after_live_override_authorization = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(
        live_override_authorization.ok() &&
            after_live_override_authorization["hstream_ui"]["stitching_calibration"]["status"].as<std::string>() ==
                "complete" &&
            after_live_override_authorization["rink"]["stitched_output_pending_generation"].as<std::string>() ==
                live_override_generation &&
            after_live_override_authorization["rink"]["scoreboard"]["perspective_polygon"].IsDefined(),
        "live rotation authorization must defer scoreboard invalidation until the request is committed");
    const auto newer_live_override_authorization =
        hm::stitching::authorize_live_stitched_output_rotation(root.string(), 10.0, "field-mask-live-10");
    const std::string newer_live_override_authorization_id =
        newer_live_override_authorization.ok() ? newer_live_override_authorization->authorization_id : std::string();
    const absl::Status superseded_publication_authority = hm::stitching::validate_field_mask_publication_authority(
        root.string(), live_override_generation, live_override_authorization_id);
    const absl::Status current_publication_authority = hm::stitching::validate_field_mask_publication_authority(
        root.string(), newer_live_override_generation, newer_live_override_authorization_id);
    auto held_config_lock = hm::stitching::GameConfigLock::Acquire(root);
    const auto busy_preflight_start = std::chrono::steady_clock::now();
    const absl::Status busy_preflight = held_config_lock.ok()
        ? hm::stitching::validate_field_mask_publication_authority(
              root.string(), newer_live_override_generation, newer_live_override_authorization_id)
        : held_config_lock.status();
    const auto busy_preflight_elapsed = std::chrono::steady_clock::now() - busy_preflight_start;
    if (held_config_lock.ok())
      held_config_lock->reset();
    const auto superseded_same_owner_publication = hm::stitching::save_rink_profile_with_stitched_image(
        root.string(),
        profile,
        stale_snapshot,
        "rink-run-a",
        pre_rotation_generation.ok() ? *pre_rotation_generation : std::string());
    const absl::Status unauthenticated_publication =
        hm::stitching::save_rink_profile(root.string(), profile, "rink-run-a");
    const YAML::Node after_superseded_same_owner = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(
        newer_live_override_authorization.ok() &&
            newer_live_override_authorization->pending_generation == newer_live_override_generation &&
            absl::IsAborted(superseded_publication_authority) && current_publication_authority.ok() &&
            absl::IsUnavailable(busy_preflight) && busy_preflight_elapsed < std::chrono::milliseconds(500) &&
            absl::IsAborted(superseded_same_owner_publication) && absl::IsAborted(unauthenticated_publication) &&
            after_superseded_same_owner["rink"]["stitched_output_pending_generation"].as<std::string>() ==
                newer_live_override_generation &&
            after_superseded_same_owner["rink"]["scoreboard"]["perspective_polygon"].IsDefined(),
        "a newer pending live generation must reject the completed same-owner producer without deleting scoreboard data");
    const auto current_live_override_authorization =
        hm::stitching::authorize_live_stitched_output_rotation(root.string(), 9.0, "field-mask-live-9-b");
    const std::string current_live_override_authorization_id = current_live_override_authorization.ok()
        ? current_live_override_authorization->authorization_id
        : std::string();
    const absl::Status current_live_override_commit = current_live_override_authorization.ok()
        ? hm::stitching::commit_live_stitched_output_rotation(
              root.string(),
              current_live_override_authorization->pending_generation,
              current_live_override_authorization->authorization_id)
        : current_live_override_authorization.status();
    ok &= expect(
        current_live_override_authorization.ok() &&
            current_live_override_authorization->pending_generation == live_override_generation &&
            current_live_override_commit.ok() &&
            !YAML::LoadFile((root / "config.yaml").string())["rink"]["scoreboard"]["perspective_polygon"].IsDefined(),
        "the current exact live generation must commit scoreboard invalidation after runtime acceptance");
    NvBufSurfaceParams superseded_live_surface_params{};
    hm::surface::Surface superseded_live_surface(&superseded_live_surface_params);
    const absl::Status superseded_pre_inference = hm::stitching::create_field_mask(
        root.string(),
        superseded_live_surface,
        live_override_generation,
        "rink-run-a",
        {},
        live_override_authorization_id);
    ok &= expect(
        absl::IsAborted(superseded_pre_inference),
        "field-mask inference must fence a superseded same-generation authorization before GPU readback");
    hm::stitching::RinkProfile mismatched_profile = profile;
    mismatched_profile.masks = {
        cv::Mat(12, 16, CV_8U, cv::Scalar(255)),
        cv::Mat(12, 16, CV_8U, cv::Scalar(255)),
    };
    const auto mismatched_masks = hm::stitching::save_rink_profile_with_stitched_image(
        root.string(),
        mismatched_profile,
        stale_snapshot,
        "rink-run-a",
        live_override_generation,
        current_live_override_authorization_id);
    const auto mismatched_snapshot = hm::stitching::save_rink_profile_with_stitched_image(
        root.string(),
        profile,
        cv::Mat(12, 16, CV_8UC3, cv::Scalar(1, 2, 3)),
        "rink-run-a",
        live_override_generation,
        current_live_override_authorization_id);
    const YAML::Node after_dimension_rejections = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(
        absl::IsAborted(mismatched_masks) && absl::IsAborted(mismatched_snapshot) &&
            after_dimension_rejections["rink"]["stitched_output_pending_generation"].as<std::string>() ==
                live_override_generation,
        "rink publication must reject mask and snapshot dimensions that disagree with the producer generation");
    const auto live_override_status = hm::stitching::save_rink_profile_with_stitched_image(
        root.string(),
        profile,
        stale_snapshot,
        "rink-run-a",
        live_override_generation,
        current_live_override_authorization_id);
    const YAML::Node after_live_override = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(
        !live_override_generation.empty() && live_override_status.ok() &&
            after_live_override["stitching"]["post_stitch_rotate_degrees"].as<double>() == 7.5 &&
            after_live_override["rink"]["stitched_output_generation"].as<std::string>() == live_override_generation &&
            after_live_override["rink"]["stitched_output_persisted_rotation_degrees"].as<double>() == 7.5 &&
            !after_live_override["rink"]["stitched_output_pending_generation"].IsDefined() &&
            hm::stitching::is_field_mask_configured(root.string(), live_override_generation, "rink-run-a"),
        "live runtime rotation override must publish without rewriting the persisted rotation");
    const auto delayed_same_generation_publication = hm::stitching::save_rink_profile_with_stitched_image(
        root.string(),
        profile,
        committed_snapshot,
        "rink-run-a",
        live_override_generation,
        live_override_authorization_id);
    ok &= expect(
        absl::IsAborted(delayed_same_generation_publication),
        "a delayed B1 producer must not consume B2 authority for the same stitched-output generation");

    std::string older_live_override_generation;
    if (!live_override_generation.empty() && live_override_canvas_size.ok()) {
      auto generation_lock = hm::stitching::HuginProject::RecoverAndLock(root);
      if (generation_lock.ok()) {
        auto hugin_generation = hm::stitching::HuginProject::GenerationId(root, **generation_lock);
        if (hugin_generation.ok()) {
          auto generation = hm::stitching::stitched_output_generation_id(
              *hugin_generation, 8.0, live_override_canvas_size->width, live_override_canvas_size->height);
          if (generation.ok())
            older_live_override_generation = *generation;
        }
      }
    }
    const auto late_older_publication = hm::stitching::save_rink_profile_with_stitched_image(
        root.string(), profile, committed_snapshot, "rink-run-a", older_live_override_generation);
    const YAML::Node after_late_older_publication = YAML::LoadFile((root / "config.yaml").string());
    ok &= expect(
        !older_live_override_generation.empty() && absl::IsAborted(late_older_publication) &&
            after_late_older_publication["rink"]["stitched_output_generation"].as<std::string>() ==
                live_override_generation,
        "a late older runtime rotation must not overwrite a completed newer generation");
    ok &= expect(
        hm::stitching::save_rink_profile_with_stitched_image(
            root.string(), profile, stale_snapshot, "rink-run-a", live_override_generation)
            .ok(),
        "completed publication must remain idempotent for the same producer generation");

    struct stat snapshot_before_rollback{};
    ok &= expect(
        ::stat((root / "s.png").c_str(), &snapshot_before_rollback) == 0,
        "rink rollback identity fixture must have a committed snapshot");
    ::setenv("HM_TEST_RINK_INTERRUPT_AFTER_PREPARE_SYNC", "1", 1);
    const auto interrupted_snapshot_publication =
        hm::stitching::save_rink_profile_with_stitched_image(root.string(), profile, stale_snapshot);
    ::unsetenv("HM_TEST_RINK_INTERRUPT_AFTER_PREPARE_SYNC");
    ok &= expect(
        !interrupted_snapshot_publication.ok(),
        "injected stitched-snapshot interruption must retain a recoverable rink journal");
    auto recovered_snapshot_lock = hm::stitching::GameConfigTransactionLock::Acquire(root);
    ok &= expect(recovered_snapshot_lock.ok(), "rink snapshot rollback must recover on the next config owner");
    if (recovered_snapshot_lock.ok())
      recovered_snapshot_lock->reset();
    struct stat snapshot_after_rollback{};
    ok &= expect(
        ::stat((root / "s.png").c_str(), &snapshot_after_rollback) == 0 &&
            snapshot_after_rollback.st_dev == snapshot_before_rollback.st_dev &&
            snapshot_after_rollback.st_ino == snapshot_before_rollback.st_ino &&
            snapshot_after_rollback.st_size == snapshot_before_rollback.st_size &&
            snapshot_after_rollback.st_mtim.tv_sec == snapshot_before_rollback.st_mtim.tv_sec &&
            snapshot_after_rollback.st_mtim.tv_nsec == snapshot_before_rollback.st_mtim.tv_nsec,
        "rink rollback must preserve the stitched snapshot inode identity");

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

    const fs::path replacement_seam = root / "seam_file.replacement.png";
    cv::Mat changed_seam(24, 32, CV_8U, cv::Scalar(255));
    changed_seam.colRange(16, changed_seam.cols).setTo(0);
    cv::imwrite(replacement_seam.string(), changed_seam);
    fs::rename(replacement_seam, root / "seam_file.png");
    ok &= expect(
        !hm::stitching::is_field_mask_configured(root.string()),
        "a seam-only replacement must invalidate a field mask generated from different stitched pixels");
    ok &= expect(
        hm::stitching::save_rink_profile(root.string(), profile).ok() &&
            hm::stitching::is_field_mask_configured(root.string()),
        "regenerating the profile must bind it to the replacement seam generation");

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
    std::string runtime_zero_rotation_generation;
    if (runtime_hugin_lock.ok()) {
      auto runtime_hugin_generation = hm::stitching::HuginProject::GenerationId(root, **runtime_hugin_lock);
      ok &= expect(runtime_hugin_generation.ok(), "runtime-override test must identify Hugin artifacts");
      if (runtime_hugin_generation.ok()) {
        auto generation = hm::stitching::stitched_output_generation_id(*runtime_hugin_generation, 5.123456789012345);
        ok &= expect(generation.ok(), "runtime-override test must identify the exact rotated output");
        if (generation.ok())
          runtime_override_generation = *generation;
        auto zero_rotation_generation = hm::stitching::stitched_output_generation_id(*runtime_hugin_generation, 0.0);
        ok &= expect(zero_rotation_generation.ok(), "runtime-override test must identify the unrotated output");
        if (zero_rotation_generation.ok())
          runtime_zero_rotation_generation = *zero_rotation_generation;
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
    const std::string runtime_dimensioned_generation =
        runtime_override_generation.empty() ? std::string() : runtime_override_generation + "output-size:32x24\n";
    ok &= expect(
        !runtime_dimensioned_generation.empty() &&
            hm::stitching::is_field_mask_configured(root.string(), runtime_dimensioned_generation),
        "a native-size runtime generation must accept only the dimensionless alias with the same rotation");
    ok &= expect(
        hm::stitching::is_field_mask_configured_for_stitching_config(
            root.string(), /*max_output_width=*/0, /*post_stitch_rotate_degrees=*/5.123456789012345) &&
            !hm::stitching::is_field_mask_configured_for_stitching_config(
                root.string(), /*max_output_width=*/0, /*post_stitch_rotate_degrees=*/0.0),
        "startup preflight must use the effective inherited or CLI rotation instead of private game YAML");
    ok &= expect(
        !hm::stitching::is_field_mask_configured(root.string()),
        "persisted rotation must not accidentally validate a different runtime output generation");
    YAML::Node stale_runtime_rotation = YAML::LoadFile((root / "config.yaml").string());
    stale_runtime_rotation["rink"]["stitched_output_generation"] = runtime_zero_rotation_generation;
    {
      std::ofstream output(root / "config.yaml");
      output << stale_runtime_rotation << '\n';
    }
    ok &= expect(
        !runtime_dimensioned_generation.empty() && !runtime_zero_rotation_generation.empty() &&
            !hm::stitching::is_field_mask_configured(root.string(), runtime_dimensioned_generation),
        "a native-size legacy alias must not replace the authoritative runtime rotation");
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

    const fs::path rolled_back = root / ".hstream-rink-rolled-back";
    fs::create_directories(rolled_back / "previous");
    std::ofstream(rolled_back / "state") << "ROLLED_BACK\n";
    const std::string config_before_rolled_back_cleanup = [&]() {
      std::ifstream input(root / "config.yaml", std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }();
    ok &= expect(
        hm::stitching::is_field_mask_configured(root.string()) && !fs::exists(rolled_back),
        "rolled-back rink journal must be cleanup-only");
    const std::string config_after_rolled_back_cleanup = [&]() {
      std::ifstream input(root / "config.yaml", std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }();
    ok &= expect(
        config_after_rolled_back_cleanup == config_before_rolled_back_cleanup,
        "rolled-back rink cleanup must not remove the restored generation");

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
