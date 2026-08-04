#include "hstream/src/libs/stitching/HuginProject.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

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
  ok &= expect(!hm::stitching::HuginProject::ParseCanvasSize("p f2 w12 v360\n").ok(), "missing height must fail");
  matches.resize(15);
  ok &=
      expect(!hm::stitching::HuginProject::InsertControlPoints(base, matches).ok(), "too few control points must fail");
  matches.resize(16, hm::stitching::FeatureMatch{{0.0f, 0.0f}, {1.0f, 1.0f}, 1.0f});
  matches[0].left.x = std::nanf("");
  ok &=
      expect(!hm::stitching::HuginProject::InsertControlPoints(base, matches).ok(), "non-finite coordinates must fail");

  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("hmstream-hugin-test-" + std::to_string(::getpid()));
  fs::create_directories(root / "game");
  {
    std::ofstream(root / "game" / "left.png") << "left\n";
    std::ofstream(root / "game" / "right.png") << "right\n";
  }
  const fs::path pto_gen = root / "pto_gen";
  const fs::path autooptimiser = root / "autooptimiser";
  const fs::path pano_modify = root / "pano_modify";
  const fs::path nona = root / "nona";
  const fs::path enblend = root / "enblend";
  ok &= expect(
      write_tool(
          pto_gen,
          "printf '%s\\n' '# hugin project file' 'p f2 w100 h50 v180' '# control points' "
          "'#hugin_optimizeReferenceImage 0' > hm_project.pto\n"),
      "fake pto_gen must be created");
  ok &= expect(
      write_tool(autooptimiser, "cp hm_project.pto autooptimiser_out.pto\n"), "fake autooptimiser must be created");
  ok &= expect(
      write_tool(
          pano_modify,
          "canvas= output= input=\n"
          "while [ \"$#\" -gt 0 ]; do case \"$1\" in --canvas=*) canvas=${1#*=} ;; -o) shift; output=$1 ;; *) "
          "input=$1 ;; esac; shift; done\n"
          "width=${canvas%x*}; height=${canvas#*x}\n"
          "awk -v w=\"$width\" -v h=\"$height\" '/^p / { sub(/w[0-9]+/, \"w\" w); sub(/h[0-9]+/, \"h\" h) } "
          "{ print }' \"$input\" > \"$output\"\n"),
      "fake pano_modify must be created");
  ok &= expect(
      write_tool(
          nona,
          "for file in mapping_0000.tif mapping_0000_x.tif mapping_0000_y.tif mapping_0001.tif "
          "mapping_0001_x.tif mapping_0001_y.tif; do printf x > \"$file\"; done\n"),
      "fake nona must be created");
  ok &= expect(
      write_tool(enblend, "printf x > seam_file.png\nprintf x > panorama.tif\n"), "fake enblend must be created");
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
  const auto configured = hm::stitching::HuginProject::Configure(root / "game", matches, options);
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
       }) {
    ok &= expect(fs::is_regular_file(root / "game" / artifact), "configured Hugin artifact must be published");
  }
  bool staging_left_behind = false;
  for (const auto& entry : fs::directory_iterator(root / "game")) {
    if (entry.path().filename().string().rfind(".hmstream-stitch-", 0) == 0)
      staging_left_behind = true;
  }
  ok &= expect(!staging_left_behind, "private Hugin staging directory must be cleaned");
  fs::remove_all(root);
  return ok ? 0 : 1;
}
