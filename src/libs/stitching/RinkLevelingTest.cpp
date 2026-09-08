#include "hstream/src/libs/stitching/RinkLeveling.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace leveling = hm::stitching;
using Vec = std::array<double, 3>;
constexpr double kRadians = 3.14159265358979323846 / 180.0;

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

Vec rotate_axis(Vec point, size_t axis, double angle) {
  const size_t first = (axis + 1) % 3;
  const size_t second = (axis + 2) % 3;
  const double x = point[first];
  const double y = point[second];
  point[first] = std::cos(angle) * x - std::sin(angle) * y;
  point[second] = std::sin(angle) * x + std::cos(angle) * y;
  return point;
}

Vec rotate(Vec point, const Vec& degrees, bool inverse = false) {
  if (inverse) {
    point = rotate_axis(point, 2, degrees[0] * kRadians);
    point = rotate_axis(point, 1, degrees[1] * kRadians);
    return rotate_axis(point, 0, -degrees[2] * kRadians);
  }
  point = rotate_axis(point, 0, degrees[2] * kRadians);
  point = rotate_axis(point, 1, -degrees[1] * kRadians);
  return rotate_axis(point, 2, -degrees[0] * kRadians);
}

Vec unit(Vec point) {
  const double norm = std::hypot(std::hypot(point[0], point[1]), point[2]);
  for (double& value : point)
    value /= norm;
  return point;
}

bool near(const Vec& first, const Vec& second, double tolerance = 1e-7) {
  for (size_t index = 0; index < 3; ++index) {
    if (std::abs(first[index] - second[index]) > tolerance)
      return false;
  }
  return true;
}

std::vector<leveling::RinkLevelingRayLine> posts(
    const Vec& desired,
    const Vec& published,
    const std::vector<double>& azimuths = {-65, -40, -15, 15, 40, 65}) {
  std::vector<leveling::RinkLevelingRayLine> result;
  for (double azimuth : azimuths) {
    const double azimuth_radians = azimuth * kRadians;
    const Vec bottom = unit({10 * std::cos(azimuth_radians), 10 * std::sin(azimuth_radians), -1});
    const Vec top = unit({10 * std::cos(azimuth_radians), 10 * std::sin(azimuth_radians), 2});
    result.push_back({rotate(rotate(bottom, desired, true), published), rotate(rotate(top, desired, true), published)});
  }
  return result;
}

bool test_absolute_geometry() {
  bool ok = true;
  const Vec desired{12, -35, 3};
  const Vec published{-17, -22, -8};
  const auto lines = posts(desired, published);
  const auto result = leveling::EstimateRinkLeveling(lines, published, desired[0]);
  ok &= expect(result.ok(), "Synthetic posts with a nonzero published correction must be solvable");
  if (result.ok()) {
    ok &= expect(near(result->rotation_degrees, desired), "Recover absolute pitch/roll, not an additive Euler delta");
    ok &= expect(
        result->inlier_indices.size() == lines.size() && result->rms_residual_degrees < 1e-7,
        "Every exact vertical post must agree with the fitted up direction");
  }
  const auto different_yaw = leveling::EstimateRinkLeveling(lines, published, 57);
  ok &= expect(
      different_yaw.ok() && near(different_yaw->rotation_degrees, {57, -35, 3}),
      "Vertical posts must preserve the caller's yaw rather than claim to estimate it");

  auto reversed = lines;
  for (auto& line : reversed)
    std::swap(line.first, line.second);
  const auto reversal = leveling::EstimateRinkLeveling(reversed, published, desired[0]);
  ok &= expect(
      reversal.ok() && near(reversal->rotation_degrees, desired), "Endpoint order must not change the estimated tilt");
  const auto reopened = leveling::EstimateRinkLeveling(posts(desired, desired), desired, desired[0]);
  ok &= expect(
      reopened.ok() && near(reopened->rotation_degrees, desired),
      "Reopening an already-leveled generation must not accumulate its correction");

  auto noisy = lines;
  for (size_t index = 0; index < noisy.size(); ++index) {
    noisy[index].first[index % 3] += (index % 2 ? 1 : -1) * 0.0003;
    noisy[index].second[(index + 1) % 3] += (index % 2 ? -1 : 1) * 0.0003;
  }
  const auto noise = leveling::EstimateRinkLeveling(noisy, published, desired[0]);
  ok &= expect(
      noise.ok() && near(noise->rotation_degrees, desired, 0.15), "Small picking errors must give a bounded fit");

  auto outlier = lines;
  outlier.push_back({rotate({1, 0, 0}, published), rotate(unit({1, 0.5, 0}), published)});
  const auto robust = leveling::EstimateRinkLeveling(outlier, published, desired[0]);
  ok &= expect(
      robust.ok() && near(robust->rotation_degrees, desired) && robust->inlier_indices.size() == lines.size() &&
          robust->residual_degrees.back() > 3,
      "An inconsistent horizontal mark must be excluded and reported without biasing the fit");
  return ok;
}

bool test_degeneracy() {
  bool ok = true;
  const Vec desired{0, -25, 3};
  const Vec zero{};
  auto lines = posts(desired, zero);
  ok &= expect(!leveling::EstimateRinkLeveling({}, zero, 0).ok(), "Missing posts must be rejected");
  ok &= expect(
      !leveling::EstimateRinkLeveling(std::vector<leveling::RinkLevelingRayLine>(8, lines[0]), zero, 0).ok(),
      "Repeated copies of the same post do not determine tilt");
  ok &= expect(
      !leveling::EstimateRinkLeveling(posts(desired, zero, {0, 0.1, 0.2, 0.3}), zero, 0).ok(),
      "Posts clustered at one azimuth must fail the conditioning check");
  lines[0].second = lines[0].first;
  ok &= expect(!leveling::EstimateRinkLeveling(lines, zero, 0).ok(), "Zero-length marks must be rejected");
  lines = posts(desired, zero);
  lines[0].first[0] = std::numeric_limits<double>::quiet_NaN();
  ok &= expect(!leveling::EstimateRinkLeveling(lines, zero, 0).ok(), "Nonfinite viewing rays must be rejected");
  lines = posts(desired, zero);
  lines[0].first = {0, 0, 0};
  ok &= expect(!leveling::EstimateRinkLeveling(lines, zero, 0).ok(), "Empty viewing rays must be rejected");
  ok &= expect(
      !leveling::EstimateRinkLeveling(posts(desired, zero), {0, 181, 0}, 0).ok() &&
          !leveling::EstimateRinkLeveling(posts(desired, zero), zero, std::numeric_limits<double>::infinity()).ok(),
      "Invalid provenance or preserved yaw must be rejected");
  return ok;
}

const std::string kPto =
    "# hugin project file\n"
    "p f19 w4254 h1992 v180 S10,4200,30,1900 P\"100 0 0\" n\"TIFF_m c:LZW r:CROP\"\n"
    "m i0\n"
    "i w100 h100 f0 v90 r0 p0 y0 a0 b0 c0 d0 e0 TrX0 TrY0 TrZ0 n\"a TrX1 name.png\"\n"
    "i w200 h100 f0 v=0 r3 p8 y30 a=0 b=0 c=0 d=0 e=0 TrX=0 TrY=0 TrZ=0 n\"right.png\"\n";

bool test_project_and_transport() {
  bool ok = true;
  const auto prepared = leveling::PrepareRinkLevelingProject(kPto);
  ok &= expect(prepared.ok(), "Valid linked Hugin calibration and quoted filenames must be accepted");
  if (!prepared.ok())
    return false;
  ok &= expect(
      prepared->image_sizes == std::vector<std::array<size_t, 2>>({{100, 100}, {200, 100}}) &&
          prepared->pto.find("p f2 w3600 h1800 v360") != std::string::npos &&
          prepared->pto.find("P\"100") == std::string::npos && prepared->pto.find("S10,") == std::string::npos &&
          prepared->pto.substr(prepared->pto.find("m i0")) == kPto.substr(kPto.find("m i0")),
      "Only output projection/crop parameters may change; camera calibration must remain byte-identical");
  for (const auto& malformed :
       {std::string(),
        kPto + "p f0 w100 h100 v90\n",
        std::string("p f0 w100 h100 v90\ni w0 h100 n\"x\"\n"),
        std::string("p f0 w100 h100 v90\ni w100 h100 TrX1 n\"x\"\n"),
        std::string("p f0 w100 h100 v90\ni w100 h100 TrX=9 n\"x\"\n"),
        std::string("p f0 w100 h100 v90\ni w100 h100 n\"x\n")}) {
    ok &= expect(!leveling::PrepareRinkLevelingProject(malformed).ok(), "Malformed or translated projects must fail");
  }
  std::vector<leveling::RinkLevelingLine> marks{
      {0, {49.5, 49.5}, {49.5, 20}}, {1, {120.25, 30.5}, {130.75, 70}}, {0, {10, 10}, {10, 80}}};
  const auto formatted = leveling::FormatRinkLevelingPoints(marks, prepared->image_sizes);
  ok &= expect(
      formatted.ok() && *formatted == "0 49.5 49.5\n0 49.5 20\n1 120.25 30.5\n1 130.75 70\n0 10 10\n0 10 80\n",
      "Triplets must preserve camera identity and subpixel coordinates");
  marks[0].first[0] = 100;
  ok &= expect(
      !leveling::FormatRinkLevelingPoints(marks, prepared->image_sizes).ok(), "Out-of-image endpoints must fail");
  marks[0].first[0] = 0;
  marks[0].image_index = 2;
  ok &= expect(
      !leveling::FormatRinkLevelingPoints(marks, prepared->image_sizes).ok(), "Unknown source cameras must fail");
  const std::string sphere = "1799.5 899.5\n1799.5 599.5\n2699.5 899.5\n899.5 899.5\n1799.5 -0.5\n1799.5 1799.5\n";
  const auto rays = leveling::ParseRinkLevelingRays(sphere, 3);
  ok &= expect(
      rays.ok() && near((*rays)[0].first, {1, 0, 0}) && near((*rays)[1].first, {0, -1, 0}) &&
          near((*rays)[1].second, {0, 1, 0}) && near((*rays)[2].first, {0, 0, 1}) &&
          near((*rays)[2].second, {0, 0, -1}),
      "Equirectangular half-pixel centers must use Hugin's x-forward/y-left/z-up coordinates");
  for (const auto& malformed : {std::string("-1 -1\n"), std::string("nan 50\n"), sphere + "0 0\n", sphere + "error"})
    ok &= expect(!leveling::ParseRinkLevelingRays(malformed, 3).ok(), "Invalid or extra transform output must fail");
  return ok;
}

bool test_delta() {
  bool ok = true;
  for (const auto& pair : std::vector<std::pair<Vec, Vec>>{
           {{-17, -22, -8}, {12, -35, 3}},
           {{0, 0, 0}, {23, 90, 7}},
           {{0, 0, 0}, {-13, -90, 9}},
           {{15, 13, -9}, {15, 13, -9}}}) {
    const auto delta = leveling::RinkLevelingRotationDelta(pair.first, pair.second);
    ok &= expect(delta.ok(), "Finite rotation pairs must produce a preview delta");
    if (!delta.ok())
      continue;
    for (const Vec probe : {Vec{1, 0, 0}, Vec{0, 1, 0}, Vec{0, 0, 1}}) {
      ok &= expect(
          near(rotate(rotate(probe, pair.first), *delta), rotate(probe, pair.second)),
          "Applying the preview delta after the old pose must equal the desired absolute pose");
    }
  }
  ok &= expect(!leveling::RinkLevelingRotationDelta({0, 0, 0}, {0, 999, 0}).ok(), "Invalid preview angles must fail");
  return ok;
}

bool has_tool(const std::string& name) {
  const char* path = std::getenv("PATH");
  std::istringstream directories(path ? path : "");
  std::string directory;
  while (std::getline(directories, directory, ':')) {
    if (::access((std::filesystem::path(directory) / name).c_str(), X_OK) == 0)
      return true;
  }
  return false;
}

bool run_tool(const std::vector<std::string>& arguments, const std::filesystem::path& directory) {
  const pid_t child = ::fork();
  if (child < 0)
    return false;
  if (child == 0) {
    const int input = ::open((directory / "stdin").c_str(), O_RDONLY);
    const int output = ::open((directory / "stdout").c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (input < 0 || output < 0 || ::dup2(input, STDIN_FILENO) < 0 || ::dup2(output, STDOUT_FILENO) < 0)
      ::_exit(125);
    ::close(input);
    ::close(output);
    std::vector<char*> command;
    for (const auto& argument : arguments)
      command.push_back(const_cast<char*>(argument.c_str()));
    command.push_back(nullptr);
    ::execvp(command[0], command.data());
    ::_exit(126);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child)
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (waited < 0)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ::kill(child, SIGKILL);
  ::waitpid(child, &status, 0);
  return false;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool test_real_hugin() {
  if (!has_tool("pano_trafo") || !has_tool("pano_modify")) {
    std::cout << "SKIP: optional installed Hugin integration (pano_trafo/pano_modify unavailable)\n";
    return true;
  }
  const auto directory = std::filesystem::temp_directory_path() / ("hstream-leveling-" + std::to_string(::getpid()));
  std::filesystem::create_directory(directory);
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{directory};
  const auto prepared = leveling::PrepareRinkLevelingProject(kPto);
  if (!prepared.ok())
    return false;
  std::ofstream(directory / "sphere.pto") << prepared->pto;
  const std::vector<leveling::RinkLevelingLine> marks{
      {0, {49.5, 49.5}, {49.5, 20}}, {0, {20, 49.5}, {20, 20}}, {0, {80, 49.5}, {80, 20}}};
  const auto formatted = leveling::FormatRinkLevelingPoints(marks, prepared->image_sizes);
  if (!formatted.ok())
    return false;
  std::ofstream(directory / "stdin") << *formatted;
  if (!expect(
          run_tool({"pano_trafo", (directory / "sphere.pto").string()}, directory), "Installed pano_trafo must run"))
    return false;
  const auto baseline = leveling::ParseRinkLevelingRays(read_file(directory / "stdout"), 3);
  if (!expect(
          baseline.ok() && near((*baseline)[0].first, {1, 0, 0}),
          "Real Hugin must agree with the half-pixel convention"))
    return false;
  if (!expect(
          run_tool(
              {"pano_modify",
               "--rotate=23,-31,7",
               "--output=" + (directory / "rotated.pto").string(),
               (directory / "sphere.pto").string()},
              directory) &&
              run_tool({"pano_trafo", (directory / "rotated.pto").string()}, directory),
          "Installed Hugin must transform a nontrivial yaw/pitch/roll"))
    return false;
  const auto rotated = leveling::ParseRinkLevelingRays(read_file(directory / "stdout"), 3);
  if (!expect(rotated.ok(), "Rotated Hugin output must parse"))
    return false;
  bool ok = true;
  for (size_t index = 0; index < baseline->size(); ++index) {
    ok &= expect(
        near((*rotated)[index].first, rotate((*baseline)[index].first, {23, -31, 7}), 2e-8) &&
            near((*rotated)[index].second, rotate((*baseline)[index].second, {23, -31, 7}), 2e-8),
        "Real Hugin camera-space rotations must match the solver's convention at off-center pixels");
  }
  return ok;
}

} // namespace

int main() {
  bool ok = test_absolute_geometry();
  ok &= test_degeneracy();
  ok &= test_project_and_transport();
  ok &= test_delta();
  ok &= test_real_hugin();
  if (ok)
    std::cout << "RinkLeveling tests passed\n";
  return ok ? 0 : 1;
}
