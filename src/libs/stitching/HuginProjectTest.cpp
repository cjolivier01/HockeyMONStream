#include "hstream/src/libs/stitching/HuginProject.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <tiffio.h>
#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool write_tool(const std::filesystem::path& path, const std::string& body) {
  std::ofstream output(path);
  output << "#!/bin/sh\nset -eu\n" << body;
  output.close();
  std::error_code error;
  std::filesystem::permissions(
      path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace,
      error);
  return output.good() && !error;
}

bool write_spatial_tiff_tags(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    float x_position,
    float resolution) {
  TIFF* tif = TIFFOpen(path.c_str(), "w");
  if (tif == nullptr)
    return false;
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, height);
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, resolution);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, resolution);
  TIFFSetField(tif, TIFFTAG_XPOSITION, x_position);
  TIFFSetField(tif, TIFFTAG_YPOSITION, 0.0f);
  std::vector<unsigned char> row(width, 127);
  bool ok = true;
  for (uint32_t y = 0; y < height; ++y)
    ok &= TIFFWriteScanline(tif, row.data(), y, 0) >= 0;
  TIFFClose(tif);
  return ok;
}

bool write_remap_pair(
    const std::filesystem::path& directory,
    const std::string& prefix,
    int width,
    int height,
    bool degenerate = false) {
  cv::Mat x(height, width, CV_16U);
  cv::Mat y(height, width, CV_16U);
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) {
      x.at<uint16_t>(row, column) = static_cast<uint16_t>(degenerate ? 12 : column);
      y.at<uint16_t>(row, column) = static_cast<uint16_t>(degenerate ? 12 : row);
    }
  }
  return cv::imwrite((directory / (prefix + "_x.tif")).string(), x) &&
      cv::imwrite((directory / (prefix + "_y.tif")).string(), y);
}

} // namespace

int main() {
  bool ok = true;
  std::vector<hm::stitching::FeatureMatch> matches;
  for (int i = 0; i < 16; ++i) {
    matches.push_back({{i + 0.25f, i + 1.5f}, {i + 2.75f, i + 3.125f}, 0.9f});
  }
  const std::string base =
      "# hugin project file\n"
      "p f2 w12092 h9267 v360\n"
      "# control points\n"
      "c n0 N1 x999 y999 X999 Y999 t0\n"
      "#hugin_optimizeReferenceImage 0\n";
  auto inserted = hm::stitching::HuginProject::InsertControlPoints(base, matches);
  ok &= expect(inserted.ok(), "valid control points must insert");
  if (inserted.ok()) {
    ok &= expect(
        inserted->find("x0.25 y1.5 X2.75 Y3.125 t0") != std::string::npos, "PTO decimals must be locale independent");
    ok &= expect(inserted->find("x999") == std::string::npos, "old PTO control points must be removed");
    size_t count = 0;
    for (size_t at = 0; (at = inserted->find("\nc n0 N1 ", at)) != std::string::npos; at += 2)
      ++count;
    ok &= expect(count == matches.size(), "every selected match must produce one control point");
  }
  auto canvas = hm::stitching::HuginProject::ParseCanvasSize(base);
  ok &= expect(canvas.ok() && canvas->first == 12092 && canvas->second == 9267, "PTO canvas dimensions must parse");
  auto projection = hm::stitching::HuginProject::ParseProjection(base);
  ok &= expect(projection.ok() && *projection == 2, "PTO panorama projection must parse");
  ok &= expect(!hm::stitching::HuginProject::ParseProjection("p w12 h6\n").ok(), "missing projection must fail");
  ok &= expect(!hm::stitching::HuginProject::ParseCanvasSize("p f2 w12 v360\n").ok(), "missing height must fail");
  const std::string pose_project =
      "i w5312 h2988 f0 r-1.25e-2 p2.5 y-43.75 Tpy0 n\"left.png\"\n"
      "i w5312 h2988 f0 r=0 p=0 y=0 r0.125 p-4.25 y43.5 n\"right.png\"\n";
  auto pose = hm::stitching::HuginProject::ParseCameraPose(pose_project, 1);
  ok &= expect(
      pose.ok() && std::abs(pose->roll - 0.125) < 1e-12 && std::abs(pose->pitch + 4.25) < 1e-12 &&
          std::abs(pose->yaw - 43.5) < 1e-12,
      "PTO camera pose must parse without confusing linked or translation tokens");
  ok &= expect(
      !hm::stitching::HuginProject::ParseCameraPose("i w1 h1 rnan p0 y0 n\"bad.png\"\n", 0).ok(),
      "non-finite PTO camera pose must fail");
  ok &=
      expect(!hm::stitching::HuginProject::ParseCameraPose(pose_project, 2).ok(), "missing PTO image index must fail");
  matches.resize(15);
  ok &=
      expect(!hm::stitching::HuginProject::InsertControlPoints(base, matches).ok(), "too few control points must fail");
  matches.resize(16, hm::stitching::FeatureMatch{{0.0f, 0.0f}, {1.0f, 1.0f}, 1.0f});
  matches[0].left.x = std::nanf("");
  ok &=
      expect(!hm::stitching::HuginProject::InsertControlPoints(base, matches).ok(), "non-finite coordinates must fail");

  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("hstream-hugin-test-" + std::to_string(::getpid()));
  fs::create_directories(root / "game");
  ok &= expect(
      cv::imwrite((root / "game" / "left.png").string(), cv::Mat(48, 64, CV_8UC3, cv::Scalar(1, 2, 3))),
      "left source fixture must exist");
  ok &= expect(
      cv::imwrite((root / "game" / "right.png").string(), cv::Mat(48, 64, CV_8UC3, cv::Scalar(4, 5, 6))),
      "right source fixture must exist");
  const fs::path pto_gen = root / "pto_gen";
  const fs::path autooptimiser = root / "autooptimiser";
  const fs::path pano_modify = root / "pano_modify";
  const fs::path nona = root / "nona";
  const fs::path enblend = root / "enblend";
  const fs::path fixtures = root / "fixtures";
  fs::create_directories(fixtures);
  constexpr float boundary_resolution = 8033.26416015625f;
  ok &= expect(
      write_spatial_tiff_tags(fixtures / "mapping_0000.tif", 40, 32, 3490.974853515625f, boundary_resolution),
      "first fake mapping must exist");
  ok &= expect(
      write_spatial_tiff_tags(fixtures / "mapping_0001.tif", 40, 32, 3490.97509765625f, boundary_resolution),
      "second fake mapping must exist");
  ok &= expect(write_remap_pair(fixtures, "mapping_0000", 40, 32), "first fake CV_16U remap must exist");
  ok &= expect(write_remap_pair(fixtures, "mapping_0001", 40, 32), "second fake CV_16U remap must exist");
  cv::Mat seam(32, 42, CV_8U, cv::Scalar(0));
  seam.colRange(21, 42).setTo(255);
  ok &= expect(cv::imwrite((fixtures / "seam_file.png").string(), seam), "fake seam must exist");
  ok &= expect(
      cv::imwrite((fixtures / "panorama.tif").string(), cv::Mat(32, 42, CV_8UC3, cv::Scalar(1, 2, 3))),
      "fake panorama must exist");
  ok &= expect(
      write_tool(
          pto_gen,
          "printf '%s\\n' '# hugin project file' 'p f2 w100 h50 v180' '# control points' "
          "'#hugin_optimizeReferenceImage 0' > hm_project.pto\n"),
      "fake pto_gen must be created");
  ok &= expect(
      write_tool(
          autooptimiser,
          "test \"$*\" = '-n -l -q -o autooptimiser_out.pto hm_project.pto'\n"
          "cp \"$6\" \"$5\"\n"
          "printf '%s\\n' 'Average (rms) distance between Controlpoints' 'after 1 iteration(s): 1.25 units'\n"),
      "fake autooptimiser must be created");
  ok &= expect(
      write_tool(
          pano_modify,
          "canvas= projection= output= input=\n"
          "while [ \"$#\" -gt 0 ]; do case \"$1\" in --canvas=*) canvas=${1#*=} ;; --projection=*) "
          "projection=${1#*=} ;; --fov=*) ;; -o) shift; output=$1 ;; *) "
          "input=$1 ;; esac; shift; done\n"
          "if [ \"$canvas\" = AUTO ]; then width=100; height=50; else width=${canvas%x*}; height=${canvas#*x}; fi\n"
          "awk -v w=\"$width\" -v h=\"$height\" -v p=\"$projection\" '/^p / { if (p != \"\") "
          "sub(/f[0-9]+/, \"f\" p); sub(/w[0-9]+/, \"w\" w); sub(/h[0-9]+/, \"h\" h) } "
          "{ print }' \"$input\" > \"$output\"\n"),
      "fake pano_modify must be created");
  ok &= expect(
      write_tool(
          nona,
          "for file in mapping_0000.tif mapping_0000_x.tif mapping_0000_y.tif mapping_0001.tif "
          "mapping_0001_x.tif mapping_0001_y.tif; do cp '" +
              fixtures.string() + "/'$file \"$file\"; done\n"),
      "fake nona must be created");
  ok &= expect(
      write_tool(
          enblend,
          "cp '" + fixtures.string() +
              "/seam_file.png' seam_file.png\n"
              "cp '" +
              fixtures.string() + "/panorama.tif' panorama.tif\n"),
      "fake enblend must be created");
  ::setenv("HM_PTO_GEN", pto_gen.c_str(), 1);
  ::setenv("HM_AUTOOPTIMISER", autooptimiser.c_str(), 1);
  ::setenv("HM_PANO_MODIFY", pano_modify.c_str(), 1);
  ::setenv("HM_NONA", nona.c_str(), 1);
  ::setenv("HM_ENBLEND", enblend.c_str(), 1);
  matches.clear();
  for (int i = 0; i < 16; ++i) {
    matches.push_back({{i + 0.25f, i + 1.5f}, {i + 2.75f, i + 3.125f}, 0.9f});
  }
  hm::stitching::HuginProject::Options options;
  options.max_canvas_dimension = 64;
  fs::create_directories(root / "private-inputs");
  ok &= expect(
      cv::imwrite((root / "private-inputs" / "left.png").string(), cv::Mat(48, 64, CV_8UC3, cv::Scalar(11, 12, 13))),
      "private left calibration input must exist");
  ok &= expect(
      cv::imwrite((root / "private-inputs" / "right.png").string(), cv::Mat(48, 64, CV_8UC3, cv::Scalar(21, 22, 23))),
      "private right calibration input must exist");
  const auto configured = hm::stitching::HuginProject::Configure(
      root / "game", root / "private-inputs" / "left.png", root / "private-inputs" / "right.png", matches, options);
  if (!configured.ok())
    std::cerr << configured << '\n';
  ok &= expect(configured.ok(), "fake Hugin toolchain must complete orchestration");
  if (configured.ok()) {
    std::ifstream optimized(root / "game" / "autooptimiser_out.pto");
    const std::string contents((std::istreambuf_iterator<char>(optimized)), std::istreambuf_iterator<char>());
    const auto scaled = hm::stitching::HuginProject::ParseCanvasSize(contents);
    ok &= expect(
        scaled.ok() && scaled->first == 64 && scaled->second == 32,
        "portable pano_modify scaling must cap the Hugin canvas");
    ok &= expect(contents.find("p f1 ") != std::string::npos, "Hugin output projection must remain cylindrical");
    ok &= expect(
        contents.find("# specify variables\nv r1\nv p1\nv y1\nv\n") != std::string::npos,
        "Hugin optimization must remain restricted to second-camera roll/pitch/yaw");
    ok &=
        expect(contents.find("v r0") == std::string::npos, "unrequested Hugin optimization variables must be removed");
    const cv::Mat published_seam = cv::imread((root / "game" / "seam_file.png").string(), cv::IMREAD_GRAYSCALE);
    ok &= expect(
        published_seam.size() == cv::Size(42, 32),
        "Hugin validation must match hm-cupano's float placement arithmetic at a pixel boundary");
    const cv::Mat published_left = cv::imread((root / "game" / "left.png").string(), cv::IMREAD_COLOR);
    const cv::Mat published_right = cv::imread((root / "game" / "right.png").string(), cv::IMREAD_COLOR);
    ok &= expect(
        !published_left.empty() && published_left.at<cv::Vec3b>(0, 0) == cv::Vec3b(11, 12, 13),
        "Hugin publication must atomically install its private left input");
    ok &= expect(
        !published_right.empty() && published_right.at<cv::Vec3b>(0, 0) == cv::Vec3b(21, 22, 23),
        "Hugin publication must atomically install its private right input");
  }
  for (const char* artifact : {
           "hm_project.pto",
           "autooptimiser_out.pto",
           "mapping_0000.tif",
           "mapping_0000_x.tif",
           "mapping_0000_y.tif",
           "mapping_0001.tif",
           "mapping_0001_x.tif",
           "mapping_0001_y.tif",
           "seam_file.png",
           "panorama.tif",
           "left.png",
           "right.png",
       }) {
    ok &= expect(fs::is_regular_file(root / "game" / artifact), "configured Hugin artifact must be published");
  }
  const auto previous_project = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ::setenv("HM_TEST_STITCH_INTERRUPT_AFTER_PREPARE_SYNC", "1", 1);
  const auto interrupted_before_publication = hm::stitching::HuginProject::Configure(root / "game", matches, options);
  ::unsetenv("HM_TEST_STITCH_INTERRUPT_AFTER_PREPARE_SYNC");
  ok &= expect(
      !interrupted_before_publication.ok(), "injected interruption after durable preparation must stop publication");
  bool durable_prepared_journal = false;
  for (const auto& entry : fs::directory_iterator(root / "game")) {
    if (entry.is_directory() && entry.path().filename().string().rfind(".hstream-stitch-", 0) == 0)
      durable_prepared_journal = true;
  }
  ok &= expect(durable_prepared_journal, "durably prepared Hugin publication must retain its recovery journal");
  const auto project_before_recovery = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(
      project_before_recovery == previous_project,
      "interruption after durable preparation must happen before replacing root artifacts");
  ok &= expect(
      hm::stitching::HuginProject::Recover(root / "game").ok(),
      "durably prepared Hugin publication must recover on the next owner");
  const auto project_after_recovery = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(project_after_recovery == previous_project, "prepared-only Hugin recovery must preserve the generation");
  ok &= expect(
      write_remap_pair(fixtures, "mapping_0000", 40, 32, true) &&
          write_remap_pair(fixtures, "mapping_0001", 40, 32, true),
      "degenerate remap fixtures must exist");
  const auto degenerate = hm::stitching::HuginProject::Configure(root / "game", matches, options);
  ok &= expect(!degenerate.ok(), "constant Hugin remaps must be rejected before publication");
  ok &= expect(
      write_remap_pair(fixtures, "mapping_0000", 40, 32) && write_remap_pair(fixtures, "mapping_0001", 40, 32),
      "valid remap fixtures must be restored");
  const auto after_degenerate = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(previous_project == after_degenerate, "degenerate remaps must preserve the prior Hugin generation");
  ok &= expect(
      write_tool(
          nona,
          "for file in mapping_0000.tif mapping_0000_x.tif mapping_0000_y.tif mapping_0001.tif "
          "mapping_0001_x.tif mapping_0001_y.tif; do printf x > \"$file\"; done\n"),
      "invalid fake nona must be created");
  const auto invalid = hm::stitching::HuginProject::Configure(root / "game", matches, options);
  ok &= expect(!invalid.ok(), "undecodable Hugin remaps must be rejected before publication");
  const auto preserved_project = [&]() {
    std::ifstream input(root / "game" / "autooptimiser_out.pto", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }();
  ok &= expect(previous_project == preserved_project, "failed Hugin validation must preserve the prior generation");
  ok &= expect(
      fs::file_size(root / "game" / "mapping_0000.tif") > 1, "failed Hugin validation must not publish a corrupt TIFF");

  const fs::path interrupted = root / "game" / ".hstream-stitch-interrupted";
  fs::create_directories(interrupted / "previous");
  const std::vector<std::string> artifact_names = {
      "left.png",
      "right.png",
      "hm_project.pto",
      "autooptimiser_out.pto",
      "mapping_0000.tif",
      "mapping_0000_x.tif",
      "mapping_0000_y.tif",
      "mapping_0001.tif",
      "mapping_0001_x.tif",
      "mapping_0001_y.tif",
      "seam_file.png",
      "panorama.tif",
  };
  std::vector<fs::file_time_type> expected_mtimes;
  const auto generation_start = fs::file_time_type::clock::now() - std::chrono::minutes(1);
  {
    std::ofstream manifest(interrupted / "artifacts");
    for (size_t index = 0; index < artifact_names.size(); ++index) {
      const std::string& name = artifact_names[index];
      const auto modified = generation_start + std::chrono::seconds(index);
      fs::last_write_time(root / "game" / name, modified);
      expected_mtimes.push_back(fs::last_write_time(root / "game" / name));
      manifest << name << '\n';
      fs::copy_file(root / "game" / name, interrupted / "previous" / name);
      fs::last_write_time(interrupted / "previous" / name, expected_mtimes.back());
    }
    std::ofstream(interrupted / "state") << "PREPARED\n";
  }
  std::ofstream(root / "game" / "mapping_0000.tif", std::ios::trunc) << 'x';
  fs::remove(root / "game" / "autooptimiser_out.pto");
  const auto recovered = hm::stitching::HuginProject::Recover(root / "game");
  ok &= expect(recovered.ok(), "prepared Hugin publication must recover after an interrupted publish");
  ok &= expect(!fs::exists(interrupted), "recovered Hugin transaction must be cleaned");
  ok &= expect(
      fs::file_size(root / "game" / "mapping_0000.tif") > 1 &&
          fs::is_regular_file(root / "game" / "autooptimiser_out.pto"),
      "Hugin recovery must restore the complete prior generation");
  for (size_t index = 0; index < artifact_names.size(); ++index) {
    ok &= expect(
        fs::last_write_time(root / "game" / artifact_names[index]) == expected_mtimes[index],
        "Hugin recovery must preserve dependency timestamps");
  }
  ok &= expect(
      fs::last_write_time(root / "game" / "right.png") < fs::last_write_time(root / "game" / "hm_project.pto") &&
          fs::last_write_time(root / "game" / "hm_project.pto") <
              fs::last_write_time(root / "game" / "autooptimiser_out.pto") &&
          fs::last_write_time(root / "game" / "autooptimiser_out.pto") <
              fs::last_write_time(root / "game" / "mapping_0000.tif"),
      "restored Hugin dependency ordering must remain usable");

  const fs::path malformed = root / "game" / ".hstream-stitch-malformed";
  fs::create_directories(malformed / "previous");
  std::ofstream(malformed / "state") << "PREPARE\n";
  const auto malformed_recovery = hm::stitching::HuginProject::Recover(root / "game");
  ok &= expect(!malformed_recovery.ok(), "unknown Hugin transaction state must fail closed");
  ok &= expect(fs::exists(malformed), "unknown Hugin transaction state must preserve its journal");
  ok &= expect(
      fs::is_regular_file(root / "game" / "autooptimiser_out.pto"),
      "unknown Hugin transaction state must not touch the committed generation");
  fs::remove_all(malformed);

  const fs::path multiline = root / "game" / ".hstream-stitch-multiline";
  fs::create_directories(multiline / "previous");
  std::ofstream(multiline / "state") << "PREPARED\n\nCOMMITTED\n";
  const auto multiline_recovery = hm::stitching::HuginProject::Recover(root / "game");
  ok &= expect(!multiline_recovery.ok(), "multiline Hugin transaction state must fail closed");
  ok &= expect(fs::exists(multiline), "multiline Hugin transaction state must preserve its journal");
  ok &= expect(
      fs::is_regular_file(root / "game" / "autooptimiser_out.pto"),
      "multiline Hugin transaction state must not touch the committed generation");
  fs::remove_all(multiline);

  const fs::path nonregular = root / "game" / ".hstream-stitch-nonregular";
  fs::create_directories(nonregular / "state");
  const auto nonregular_recovery = hm::stitching::HuginProject::Recover(root / "game");
  ok &= expect(!nonregular_recovery.ok(), "non-regular Hugin transaction state must fail closed");
  ok &= expect(fs::exists(nonregular), "non-regular Hugin transaction state must preserve its journal");
  fs::remove_all(nonregular);

  const fs::path unprepared = root / "game" / ".hstream-stitch-unprepared";
  fs::create_directories(unprepared);
  std::ofstream(unprepared / "temporary") << "not published\n";
  ok &= expect(
      hm::stitching::HuginProject::Recover(root / "game").ok() && !fs::exists(unprepared),
      "unprepared Hugin staging without publication metadata must be cleaned");

  const fs::path committed = root / "game" / ".hstream-stitch-committed";
  fs::create_directories(committed);
  std::ofstream(committed / "state") << "COMMITTED\n";
  ok &= expect(
      hm::stitching::HuginProject::Recover(root / "game").ok() && !fs::exists(committed),
      "committed Hugin journal must be cleaned without rollback");

  auto reader_lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
  ok &= expect(reader_lock.ok(), "Hugin reader must acquire the artifact lock");
  std::atomic<bool> second_reader_entered{false};
  std::atomic<bool> second_reader_ok{false};
  std::thread second_reader([&] {
    auto lock = hm::stitching::HuginProject::RecoverAndLock(root / "game");
    second_reader_ok = lock.ok();
    second_reader_entered = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ok &= expect(!second_reader_entered, "Hugin publication lock must remain held while a reader decodes artifacts");
  if (reader_lock.ok())
    reader_lock->reset();
  second_reader.join();
  ok &= expect(second_reader_entered && second_reader_ok, "waiting Hugin reader must proceed after lock release");
  bool staging_left_behind = false;
  for (const auto& entry : fs::directory_iterator(root / "game")) {
    if (entry.path().filename().string().rfind(".hstream-stitch-", 0) == 0)
      staging_left_behind = true;
  }
  ok &= expect(!staging_left_behind, "private Hugin staging directory must be cleaned");
  fs::remove_all(root);
  return ok ? 0 : 1;
}
