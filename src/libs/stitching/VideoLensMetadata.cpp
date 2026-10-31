#include "hstream/src/libs/stitching/VideoLensMetadata.h"

#include <sys/stat.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "hstream/src/libs/stitching/CalibrationFrameExif.h"

namespace hm::stitching {
namespace {
using Bytes = std::vector<unsigned char>;
constexpr size_t kMaxMetadata = 4 * 1024 * 1024;
constexpr double kPi = 3.14159265358979323846;
struct Unsupported : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
uint64_t integer(const unsigned char* p, size_t n, bool little = false) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i)
    v = (v << 8) | p[little ? n - i - 1 : i];
  return v;
}
double real(const unsigned char* p, bool little = false) {
  const uint32_t bits = integer(p, 4, little);
  float v;
  std::memcpy(&v, &bits, 4);
  require(std::isfinite(v), "Nonfinite lens coefficient");
  return v;
}
std::string string(const Bytes& b) {
  const auto end = std::find(b.begin(), b.end(), 0);
  require(static_cast<size_t>(end - b.begin()) <= 4096, "Lens metadata string exceeds limit");
  return std::string(b.begin(), end);
}
struct Field {
  unsigned type{0};
  uint64_t value{0};
  Bytes bytes;
};
using Fields = std::map<unsigned, Field>;
Fields protobuf(const Bytes& data) {
  require(data.size() <= kMaxMetadata, "Lens metadata exceeds size limit");
  Fields fields;
  size_t p = 0, count = 0;
  auto varint = [&]() {
    uint64_t v = 0;
    for (unsigned shift = 0; shift < 64 && p < data.size(); shift += 7) {
      const auto c = data[p++];
      require(shift != 63 || c <= 1, "Invalid protobuf integer");
      v |= uint64_t(c & 127) << shift;
      if (!(c & 128))
        return v;
    }
    throw std::runtime_error("Truncated protobuf integer");
  };
  while (p < data.size()) {
    require(++count <= 4096, "Too many lens metadata fields");
    const uint64_t key = varint();
    require(key >= 8 && key >> 3 <= 0x1fffffff, "Invalid protobuf field");
    Field f;
    f.type = key & 7;
    if (f.type == 0)
      f.value = varint();
    else {
      require(f.type == 1 || f.type == 2 || f.type == 5, "Unsupported protobuf wire type");
      const auto length = f.type == 2 ? varint() : f.type == 1 ? 8 : 4;
      require(length <= data.size() - p, "Truncated protobuf field");
      f.bytes.assign(data.begin() + p, data.begin() + p + length);
      p += length;
    }
    require(fields.emplace(key >> 3, std::move(f)).second, "Repeated lens metadata field");
  }
  return fields;
}
Fields child(const Fields& f, unsigned key) {
  const auto it = f.find(key);
  if (it == f.end())
    return {};
  require(it->second.type == 2, "Invalid nested lens metadata");
  return protobuf(it->second.bytes);
}
uint64_t scalar(const Fields& f, unsigned key, uint64_t fallback = 0) {
  auto it = f.find(key);
  if (it == f.end())
    return fallback;
  require(it->second.type == 0, "Invalid scalar lens metadata");
  return it->second.value;
}
std::string text(const Fields& f, unsigned key) {
  auto it = f.find(key);
  if (it == f.end())
    return {};
  require(it->second.type == 2, "Invalid string lens metadata");
  return string(it->second.bytes);
}

// Fit radius/R = sum(q[i] * (theta/theta_max)^(i+1)). Normalizing
// the argument keeps the small least-squares system well scaled.
StitchCameraSelection fit(size_t w, size_t h, const std::function<double(double)>& angle, double extent = 0) {
  require(w >= 16 && h >= 16 && w <= 32768 && h <= 32768, "Invalid lens image dimensions");
  const double diagonal = std::max(extent, std::hypot(double(w), double(h)) / 2), radius = std::min(w, h) / 2.0;
  const double max_angle = angle(diagonal);
  require(std::isfinite(max_angle) && max_angle > 0 && max_angle < kPi, "Invalid lens angular coverage");
  std::array<std::array<double, 5>, 4> system{};
  constexpr size_t samples = 20000;
  double previous = -1;
  for (size_t i = 0; i <= samples; ++i) {
    const double r = diagonal * i / samples, theta = angle(r), x = theta / max_angle;
    require(
        std::isfinite(theta) && theta > previous && (i || std::abs(theta) < 1e-10),
        "Lens mapping is not centered and monotonic");
    previous = theta;
    std::array<double, 8> powers{};
    powers[0] = x;
    for (size_t j = 1; j < powers.size(); ++j)
      powers[j] = powers[j - 1] * x;
    for (size_t a = 0; a < 4; ++a) {
      for (size_t b = 0; b < 4; ++b)
        system[a][b] += powers[a + b + 1];
      system[a][4] += powers[a] * r / radius;
    }
  }
  for (size_t col = 0; col < 4; ++col) {
    size_t pivot = col;
    for (size_t row = col + 1; row < 4; ++row)
      if (std::abs(system[row][col]) > std::abs(system[pivot][col]))
        pivot = row;
    std::swap(system[col], system[pivot]);
    require(std::abs(system[col][col]) > 1e-10, "Degenerate lens fit");
    const double scale = system[col][col];
    for (size_t j = col; j <= 4; ++j)
      system[col][j] /= scale;
    for (size_t row = 0; row < 4; ++row) {
      if (row == col)
        continue;
      const double factor = system[row][col];
      for (size_t j = col; j <= 4; ++j)
        system[row][j] -= factor * system[col][j];
    }
  }
  auto fitted = [&](double x) {
    return x * (system[0][4] + x * (system[1][4] + x * (system[2][4] + x * system[3][4])));
  };
  double error = 0, last = -1;
  for (size_t i = 0; i <= samples; ++i) {
    const double r = diagonal * i / samples, value = fitted(angle(r) / max_angle);
    require(value > last, "Fitted lens folds at image edge");
    last = value;
    error = std::max(error, std::abs(value * radius - r));
  }
  if (error > std::max(w, h) / 7680.0)
    throw Unsupported("Lens cannot be represented accurately by Hugin a/b/c (over 1 pixel at 8K)");
  double lo = 0, hi = 1;
  require(fitted(hi) >= 1, "Lens fit does not cover image");
  for (int i = 0; i < 64; ++i) {
    const double mid = (lo + hi) / 2;
    if (fitted(mid) < 1)
      lo = mid;
    else
      hi = mid;
  }
  const double x = (lo + hi) / 2;
  StitchCameraSelection result;
  result.configuration = "auto";
  result.horizontal_fov = w * x * max_angle / radius * 180 / kPi;
  result.vertical_fov = 2 * angle(h / 2.0) * 180 / kPi;
  result.input_lens.projection = "full-frame-fisheye";
  result.input_lens.optimization = "fixed";
  result.input_lens.distortion = {system[3][4] * std::pow(x, 4), system[2][4] * std::pow(x, 3), system[1][4] * x * x};
  require(
      result.horizontal_fov > 0 && result.horizontal_fov < 360 && result.vertical_fov <= 180,
      "Unsupported lens field of view");
  for (double c : result.input_lens.distortion)
    require(std::abs(c) <= 1, "Lens coefficient exceeds supported range");
  return result;
}
std::function<double(double)> inverse(const std::function<double(double)>& radius, double max_radius) {
  double hi = 0, previous = -1;
  // Stop at the first radius covering the frame, before singularities outside it.
  for (size_t i = 0; i < 30000; ++i) {
    hi = i * kPi / 30000;
    const double r = radius(hi);
    require(std::isfinite(r) && r > previous, "Nonmonotonic source lens model");
    previous = r;
    if (r >= max_radius)
      break;
  }
  require(previous >= max_radius, "Source lens model does not cover image");
  return [radius, hi](double r) {
    if (r == 0)
      return 0.0;
    double a = 0, b = hi;
    for (int i = 0; i < 48; ++i) {
      const double mid = (a + b) / 2;
      if (radius(mid) < r)
        a = mid;
      else
        b = mid;
    }
    return (a + b) / 2;
  };
}

struct Gpmf {
  unsigned type;
  Bytes bytes;
};
using Tags = std::map<std::string, Gpmf>;
Tags gpmf(const Bytes& data) {
  Tags tags;
  for (size_t p = 0; p < data.size();) {
    if (data[p] == 0) {
      require(
          std::all_of(data.begin() + p, data.end(), [](unsigned char c) { return c == 0; }),
          "Nonzero bytes after GPMF padding");
      break;
    }
    require(tags.size() < 4096, "Too many GPMF tags");
    require(data.size() - p >= 8, "Truncated GPMF header");
    const size_t n = data[p + 5] * integer(data.data() + p + 6, 2), padded = (n + 3) & ~size_t(3);
    require(padded <= data.size() - p - 8, "Truncated GPMF payload");
    const std::string key(reinterpret_cast<const char*>(data.data() + p), 4);
    require(
        tags.emplace(key, Gpmf{data[p + 4], Bytes(data.begin() + p + 8, data.begin() + p + 8 + n)}).second,
        "Duplicate lens metadata tag");
    p += 8 + padded;
  }
  return tags;
}
std::vector<double> floats(const Tags& tags, const std::string& key) {
  auto it = tags.find(key);
  require(
      it != tags.end() && it->second.type == 'f' && it->second.bytes.size() % 4 == 0,
      "Missing/invalid GoPro lens field");
  std::vector<double> values;
  for (size_t p = 0; p < it->second.bytes.size(); p += 4)
    values.push_back(real(it->second.bytes.data() + p));
  return values;
}
double number(const Tags& t, const std::string& key) {
  auto v = floats(t, key);
  require(v.size() == 1, "Invalid GoPro scalar");
  return v[0];
}
std::string tag_text(const Tags& t, const std::string& key) {
  auto it = t.find(key);
  return it == t.end() ? "" : string(it->second.bytes);
}
VideoLensMetadata unavailable(VideoLensMetadata result, const std::string& reason) {
  result.unavailable_reason = result.model + ": " + reason;
  if (!result.horizontal_fov && !result.vertical_fov)
    result.fov_unavailable_reason = result.unavailable_reason;
  return result;
}

VideoLensMetadata parse_gopro(const Bytes& data) {
  require(data.size() <= kMaxMetadata, "GoPro metadata too large");
  Tags global, lens;
  // DEVC repeats at the top level; lens tags must stay in their device scope.
  for (size_t p = 0; p < data.size();) {
    // GoPro reserves a fixed-size GPMF atom. Chapter headers can leave an
    // arbitrary amount of zero padding, including a final four-byte word.
    if (data[p] == 0) {
      require(
          std::all_of(data.begin() + p, data.end(), [](unsigned char c) { return c == 0; }),
          "Nonzero bytes after GPMF padding");
      break;
    }
    require(data.size() - p >= 8, "Truncated GoPro device");
    size_t n = data[p + 5] * integer(data.data() + p + 6, 2), padded = (n + 3) & ~size_t(3);
    require(padded <= data.size() - p - 8, "Truncated GoPro device payload");
    if (std::memcmp(data.data() + p, "DEVC", 4) == 0 && data[p + 4] == 0) {
      auto t = gpmf(Bytes(data.begin() + p + 8, data.begin() + p + 8 + n));
      if (tag_text(t, "DVID") == "FOVL") {
        require(lens.empty(), "Multiple GoPro FOVL models");
        lens = std::move(t);
      } else if (t.count("MINF")) {
        require(global.empty(), "Multiple GoPro global settings");
        global = std::move(t);
      }
    }
    p += 8 + padded;
  }
  VideoLensMetadata result;
  result.model = tag_text(global, "MINF");
  result.stabilization = tag_text(global, "EISA");
  result.mode = tag_text(lens, "VFOV");
  auto res = global.find("VRES");
  require(
      res != global.end() && res->second.type == 'L' && res->second.bytes.size() == 8,
      "Missing GoPro video dimensions");
  result.width = integer(res->second.bytes.data(), 4);
  result.height = integer(res->second.bytes.data() + 4, 4);
  result.recording_mode = {
      {"vendor", "gopro"},
      {"model", result.model},
      {"width", std::to_string(result.width)},
      {"height", std::to_string(result.height)},
      {"digital_lens", result.mode},
      {"stabilization", result.stabilization},
      {"horizon", tag_text(global, "HCTL")},
      {"projection", tag_text(global, "PRJT")},
      {"firmware", tag_text(global, "FMWR")}};
  if (global.count("LPID"))
    result.recording_mode["lens_id"] = tag_text(global, "LPID");
  const auto fps = global.find("VFPS");
  if (fps != global.end() && fps->second.type == 'L' && fps->second.bytes.size() == 8) {
    const auto n = integer(fps->second.bytes.data(), 4), d = integer(fps->second.bytes.data() + 4, 4);
    if (n && d) {
      std::ostringstream rate;
      rate << std::fixed << std::setprecision(3) << double(n) / d;
      result.recording_mode["frame_rate"] = rate.str();
    }
  }
  if (global.count("ZOOM")) {
    std::ostringstream zoom;
    zoom << std::setprecision(9) << number(global, "ZOOM");
    result.recording_mode["zoom"] = zoom.str();
  }
  if (lens.empty())
    return unavailable(result, "no FOVL prescription or axis FOV");
  if (tag_text(global, "PRJT") != "GPRO")
    return unavailable(result, "unsupported GoPro lens projection");
  if (result.mode != "W")
    return unavailable(result, "only Wide radial metadata is supported");
  std::string eis = result.stabilization;
  std::transform(eis.begin(), eis.end(), eis.begin(), [](unsigned char c) { return std::tolower(c); });
  if (eis.find("boost") != std::string::npos || (lens.count("ABSC") && number(lens, "ABSC") != 0))
    return unavailable(result, "AutoBoost needs a changing lens model");
  for (const char* key : {"MAPX", "MAPY"})
    if (floats(lens, key) != std::vector<double>{1})
      return unavailable(result, "digital lens warp is not radial");
  result.recording_mode["warp"] = "identity";
  const double ratio = double(result.width) / result.height;
  require(
      std::abs(number(lens, "ARUW") - ratio) < 1e-5 && std::abs(number(lens, "ARWA") - ratio) < 1e-5,
      "GoPro aspect ratio does not match recorded image");
  auto read_fov = [&](const char* key, double maximum) -> std::optional<double> {
    if (!lens.count(key))
      return std::nullopt;
    const double value = number(lens, key);
    require(value > 0 && value < maximum, "Invalid GoPro axis field of view");
    return value;
  };
  result.horizontal_fov = read_fov("XFOV", 360);
  result.vertical_fov = read_fov("YFOV", 180.000001);
  result.fov_unavailable_reason = "recording omits horizontal or vertical field of view";
  for (const char* required : {"POLY", "PYCF", "ZMPL"})
    if (!lens.count(required))
      return unavailable(result, "recording omits the radial lens prescription");
  auto poly = floats(lens, "POLY");
  require(poly.size() == 8, "Unsupported GoPro polynomial order");
  const auto coefficients = lens.find("PYCF");
  require(coefficients != lens.end() && coefficients->second.bytes.size() == 64, "Missing GoPro polynomial basis");
  for (size_t i = 0; i < 8; ++i) {
    const auto* b = coefficients->second.bytes.data() + 8 * i;
    require(b[0] == 'r' && b[1] == '0' + i && b[2] == 0, "Unsupported GoPro polynomial basis");
  }
  const double z = number(lens, "ZMPL"), d = std::hypot(double(result.width), double(result.height)) / 2;
  require(z > 0 && z < 10, "Invalid GoPro zoom normalization");
  auto angle = [poly, z, d](double r) {
    double v = 0;
    for (auto it = poly.rbegin(); it != poly.rend(); ++it)
      v = v * z * r / d + *it;
    return v;
  };
  for (const auto& axis : std::vector<std::pair<std::string, double>>{
           {"XFOV", result.width / 2.0}, {"YFOV", result.height / 2.0}, {"ZFOV", d}})
    if (lens.count(axis.first))
      require(
          std::abs(number(lens, axis.first) - angle(axis.second) * 360 / kPi) < 0.25,
          "GoPro lens prescription disagrees with recorded field of view");
  // Some recordings carry the radial prescription without separate axis tags.
  // Its coverage is still usable independently of importing the correction.
  // fit() below validates the original mapping before it can reject only the
  // accuracy of its Hugin approximation.
  if (!result.horizontal_fov)
    result.horizontal_fov = angle(result.width / 2.0) * 360 / kPi;
  if (!result.vertical_fov)
    result.vertical_fov = angle(result.height / 2.0) * 360 / kPi;
  require(
      *result.horizontal_fov > 0 && *result.horizontal_fov < 360 && *result.vertical_fov > 0 &&
          *result.vertical_fov <= 180,
      "Invalid derived GoPro axis field of view");
  result.fov_unavailable_reason.clear();
  try {
    result.camera = fit(result.width, result.height, angle);
  } catch (const Unsupported& error) {
    return unavailable(result, error.what());
  }
  if (result.vertical_fov)
    result.camera.vertical_fov = *result.vertical_fov;
  return result;
}

VideoLensMetadata parse_insta(const Bytes& data) {
  const auto f = protobuf(data);
  VideoLensMetadata result;
  result.model = text(f, 2);
  if (result.model.empty())
    throw Unsupported("Insta360 metadata has no camera model");
  const auto dims = child(f, 19);
  result.width = scalar(dims, 1);
  result.height = scalar(dims, 2);
  result.mode = "FOV " + std::to_string(scalar(f, 47));
  result.stabilization = scalar(f, 42) ? "FlowState" : "Off";
  if (scalar(f, 43))
    result.mode += " / dewarped";
  if (scalar(f, 42) || scalar(f, 43))
    return unavailable(result, "FlowState/dewarping is baked in; factory lens coefficients do not describe the output");
  if (!text(f, 111).empty())
    return unavailable(result, "v6 lens model is not supported");
  std::istringstream stream(text(f, 54));
  std::string token;
  std::vector<double> values;
  while (std::getline(stream, token, '_')) {
    require(values.size() < 64, "Insta360 lens model is too large");
    size_t consumed = 0;
    double value = std::stod(token, &consumed);
    require(consumed == token.size() && std::isfinite(value), "Invalid Insta360 lens coefficient");
    values.push_back(value);
  }
  if (values.size() != 21 || values[0] != 1)
    return unavailable(result, "no supported single-lens v3 calibration");
  const auto& v = values;
  if (v[17] != result.width || v[18] != result.height || std::abs(v[2] - v[3]) > 1e-5 || v[15] != 0 || v[16] != 0)
    return unavailable(result, "cropped, asymmetric or tangential calibration needs a different lens model");
  const auto crop = child(f, 27);
  if (!crop.empty() &&
      (scalar(crop, 1) != result.width || scalar(crop, 2) != result.height || scalar(crop, 3) != result.width ||
       scalar(crop, 4) != result.height))
    return unavailable(result, "recording crop is not supported by the radial importer");
  auto radius = [v](double theta) {
    const double r = std::sin(theta) / (std::cos(theta) + v[1]), r2 = r * r;
    return v[2] * r * (1 + r2 * (v[12] + r2 * (v[13] + r2 * v[14])));
  };
  const double extent = std::hypot(std::max(v[4], result.width - v[4]), std::max(v[5], result.height - v[5]));
  const auto angle = inverse(radius, extent);
  result.horizontal_fov = (angle(v[4]) + angle(result.width - v[4])) * 180 / kPi;
  result.vertical_fov = (angle(v[5]) + angle(result.height - v[5])) * 180 / kPi;
  try {
    result.camera = fit(result.width, result.height, angle, extent);
  } catch (const Unsupported& error) {
    return unavailable(result, error.what());
  }
  result.camera.input_lens.center_offset = {
      (v[4] - result.width / 2.0) / result.width, (v[5] - result.height / 2.0) / result.height};
  if (std::abs(result.camera.input_lens.center_offset[0]) > 0.01 ||
      std::abs(result.camera.input_lens.center_offset[1]) > 0.01)
    return unavailable(result, "optical center is outside the supported radial fit margin");
  result.mode = "raw v3 / " + result.mode;
  result.stabilization = "Off";
  return result;
}

VideoLensMetadata parse_dji(const Bytes& data, size_t width, size_t height) {
  const auto root = protobuf(data), clip = child(root, 1), header = child(clip, 1);
  VideoLensMetadata result;
  result.model = text(header, 10);
  result.width = width;
  result.height = height;
  const std::string schema = text(header, 1);
  if (schema != "dvtm_ac204.proto" && schema != "dvtm_wm169.proto")
    throw Unsupported(result.model + ": unsupported DJI metadata schema " + schema);
  const auto distortion = child(clip, 3), focal = child(clip, 8), eis = child(clip, 9);
  // Proto3 omits an OFF scalar inside a present message; an absent message
  // does not establish the recording's stabilization state.
  if (!clip.count(9))
    return unavailable(result, "recording omits stabilization status");
  result.stabilization = "EIS " + std::to_string(scalar(eis, 1));
  if (scalar(eis, 1))
    return unavailable(result, "stabilization is enabled; no supported static output lens prescription");
  if (!focal.count(1) || !distortion.count(1))
    return unavailable(result, "recording omits focal length/distortion coefficients");
  require(
      focal.at(1).type == 5 && focal.at(1).bytes.size() == 4 && distortion.at(1).type == 2 &&
          distortion.at(1).bytes.size() == 16,
      "Invalid DJI lens coefficient layout");
  const double fx = real(focal.at(1).bytes.data(), true);
  require(fx > 0, "Invalid DJI focal length");
  std::array<double, 4> k;
  for (size_t i = 0; i < 4; ++i)
    k[i] = real(distortion.at(1).bytes.data() + i * 4, true);
  auto radius = [fx, k](double t) {
    double t2 = t * t;
    return fx * t * (1 + t2 * (k[0] + t2 * (k[1] + t2 * (k[2] + t2 * k[3]))));
  };
  const auto angle = inverse(radius, std::hypot(double(width), double(height)) / 2);
  result.horizontal_fov = angle(width / 2.0) * 360 / kPi;
  result.vertical_fov = angle(height / 2.0) * 360 / kPi;
  try {
    result.camera = fit(width, height, angle);
  } catch (const Unsupported& error) {
    return unavailable(result, error.what());
  }
  result.stabilization = "Off";
  result.mode = "OpenCV fisheye";
  return result;
}

class Mp4 {
 public:
  explicit Mp4(const std::filesystem::path& path) : file(path, std::ios::binary | std::ios::ate) {
    require(bool(file), "Cannot open lens metadata source");
    auto end = file.tellg();
    require(end >= 0, "Cannot size lens metadata source");
    size = end;
  }
  struct Atom {
    std::string type;
    uint64_t pos, size;
  };
  Bytes read(uint64_t pos, size_t n) {
    require(n <= kMaxMetadata && bytes_read + n <= 8 * kMaxMetadata, "MP4 metadata read limit exceeded");
    require(pos <= size && n <= size - pos, "MP4 metadata read outside file");
    bytes_read += n;
    Bytes b(n);
    file.seekg(pos);
    require(bool(file.read(reinterpret_cast<char*>(b.data()), n)), "Truncated MP4 metadata");
    return b;
  }
  std::vector<Atom> atoms(uint64_t pos, uint64_t length, const std::string& stop_after = {}) {
    require(pos <= size && length <= size - pos, "MP4 container outside file");
    const auto end = pos + length;
    std::vector<Atom> result;
    while (pos < end) {
      require(++boxes <= 4096 && end - pos >= 8, "Invalid MP4 atom count/length");
      auto b = read(pos, 8);
      uint64_t n = integer(b.data(), 4), h = 8;
      if (n == 1) {
        require(end - pos >= 16, "Truncated extended MP4 atom");
        auto extended = read(pos + 8, 8);
        n = integer(extended.data(), 8);
        h = 16;
      }
      if (n == 0)
        n = end - pos;
      require(n >= h && n <= end - pos, "MP4 atom outside container");
      result.push_back({std::string(b.begin() + 4, b.end()), pos + h, n - h});
      pos += n;
      if (result.back().type == stop_after)
        break;
    }
    return result;
  }
  void track(const Atom& atom, std::map<std::string, Atom>& leaves, unsigned depth = 0) {
    require(depth < 5, "MP4 track nesting limit");
    for (const auto& a : atoms(atom.pos, atom.size)) {
      if (a.type == "mdia" || a.type == "minf" || a.type == "stbl")
        track(a, leaves, depth + 1);
      else
        leaves.emplace(a.type, a);
    }
  }
  std::ifstream file;
  uint64_t size;
  size_t bytes_read{0}, boxes{0};
};
VideoLensMetadata read_file(const std::filesystem::path& path) {
  const auto insta = frame_exif::ReadInsta360Record(path, 1, kMaxMetadata);
  if (!insta.ok())
    throw std::runtime_error(insta.status().ToString());
  std::optional<VideoLensMetadata> vendor;
  if (insta->has_value()) {
    require(insta->value().full_size <= kMaxMetadata, "Insta360 metadata too large");
    vendor = parse_insta(insta->value().data);
  }
  Mp4 mp4(path);
  size_t width = 0, height = 0;
  std::map<std::string, Mp4::Atom> dji_track;
  // Camera metadata and video sample descriptions are contained in moov.
  // Do not interpret a legacy unboxed Insta360 trailer as more MP4 atoms.
  for (const auto& top : mp4.atoms(0, mp4.size, "moov")) {
    if (top.type != "moov")
      continue;
    for (const auto& a : mp4.atoms(top.pos, top.size)) {
      if (a.type == "udta") {
        for (const auto& g : mp4.atoms(a.pos, a.size))
          if (g.type == "GPMF") {
            require(!vendor.has_value(), "Multiple camera lens metadata blocks");
            vendor = parse_gopro(mp4.read(g.pos, g.size));
          }
      }
      if (a.type != "trak")
        continue;
      std::map<std::string, Mp4::Atom> leaves;
      mp4.track(a, leaves);
      if (!leaves.count("stsd"))
        continue;
      const auto s = leaves.at("stsd");
      if (s.size < 16)
        continue;
      const auto b = mp4.read(s.pos, std::min<uint64_t>(s.size, 100));
      const std::string format(b.begin() + 12, b.begin() + 16);
      if (format == "djmd")
        dji_track = leaves;
      if (format == "hvc1" || format == "hev1" || format == "avc1") {
        require(b.size() >= 44, "Truncated video sample description");
        require(width == 0, "Multiple video tracks need independent lens models");
        width = integer(b.data() + 40, 2);
        height = integer(b.data() + 42, 2);
      }
    }
  }
  if (vendor) {
    require(
        width != 0 && width == vendor->width && height == vendor->height,
        "Lens metadata dimensions do not match the recorded video track");
    return *vendor;
  }
  if (!dji_track.empty()) {
    const auto offsets = dji_track.find(dji_track.count("co64") ? "co64" : "stco"), sizes = dji_track.find("stsz");
    require(offsets != dji_track.end() && sizes != dji_track.end(), "DJI metadata has no sample table");
    const size_t n = offsets->first == "co64" ? 8 : 4;
    require(offsets->second.size >= 8 + n && sizes->second.size >= 12, "Truncated DJI sample table");
    auto offset = mp4.read(offsets->second.pos, 8 + n),
         size = mp4.read(sizes->second.pos, std::min<uint64_t>(sizes->second.size, 16));
    require(integer(offset.data() + 4, 4) > 0 && integer(size.data() + 8, 4) > 0, "Empty DJI sample table");
    uint64_t length = integer(size.data() + 4, 4);
    if (!length) {
      require(size.size() >= 16, "Missing DJI sample size");
      length = integer(size.data() + 12, 4);
    }
    return parse_dji(mp4.read(integer(offset.data() + 8, n), length), width, height);
  }
  throw Unsupported("No supported lens metadata; original camera files may be required");
}
template <class F>
absl::StatusOr<VideoLensMetadata> checked(F work) {
  try {
    return work();
  } catch (const Unsupported& e) {
    return absl::UnimplementedError(e.what());
  } catch (const std::exception& e) {
    return absl::DataLossError(e.what());
  }
}
} // namespace

absl::StatusOr<VideoLensMetadata> ParseVideoLensMetadata(const Bytes& data) {
  return checked([&] { return parse_gopro(data); });
}
absl::StatusOr<VideoLensMetadata> ParseInsta360LensMetadata(const Bytes& data) {
  return checked([&] { return parse_insta(data); });
}
absl::StatusOr<VideoLensMetadata> ParseDjiLensMetadata(const Bytes& data, size_t width, size_t height) {
  return checked([&] { return parse_dji(data, width, height); });
}
absl::StatusOr<VideoLensMetadata> ReadVideoLensMetadata(const std::filesystem::path& video) {
  struct stat info;
  if (stat(video.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
    return absl::NotFoundError("Source video is unavailable");
  const std::string key = std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino) + ":" +
      std::to_string(info.st_size) + ":" + std::to_string(info.st_mtim.tv_sec) + ":" +
      std::to_string(info.st_mtim.tv_nsec) + ":" + std::to_string(info.st_ctim.tv_sec) + ":" +
      std::to_string(info.st_ctim.tv_nsec);
  static std::mutex mutex;
  static std::map<std::string, absl::StatusOr<VideoLensMetadata>> cache;
  {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(key);
    if (it != cache.end())
      return it->second;
  }
  auto result = checked([&] { return read_file(video); });
  struct stat after;
  if (stat(video.c_str(), &after) != 0 || after.st_dev != info.st_dev || after.st_ino != info.st_ino ||
      after.st_size != info.st_size || after.st_mtim.tv_sec != info.st_mtim.tv_sec ||
      after.st_mtim.tv_nsec != info.st_mtim.tv_nsec || after.st_ctim.tv_sec != info.st_ctim.tv_sec ||
      after.st_ctim.tv_nsec != info.st_ctim.tv_nsec)
    return absl::AbortedError("Source video changed while reading lens metadata");
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() >= 256)
      cache.erase(cache.begin());
    cache.insert_or_assign(key, result);
  }
  return result;
}

namespace {
bool close(double a, double b) {
  return std::abs(a - b) <= 1e-6;
}

// Convert physical horizontal coverage to the selected Hugin model. The radial
// coefficients and HFOV parameter share a normalization; copying XFOV directly
// next to arbitrary a/b/c changes the represented coverage.
double hugin_fov(double coverage, size_t width, size_t height, const StitchInputLens& lens, bool to_physical = false) {
  require(
      width >= 16 && height >= 16 && coverage > 0 && coverage < (to_physical ? 360 : 180),
      "Cannot combine this FOV with the selected lens model");
  const double radius = std::min(width, height) / 2.0;
  const auto& c = lens.distortion;
  auto radial = [&](double t) { return t * (1 - c[0] - c[1] - c[2] + t * (c[2] + t * (c[1] + t * c[0]))); };
  const double ox = lens.center_offset[0] * width, oy = lens.center_offset[1] * height;
  const double edge = std::hypot(width / 2.0 + std::abs(ox), height / 2.0 + std::abs(oy)) / radius;
  // Find an invertible radial branch through all image corners, not an arbitrary
  // later root of a folding polynomial.
  double upper = 0, previous = -1;
  for (size_t i = 0; i <= 10000; ++i) {
    upper = i * 0.001;
    const double r = radial(upper);
    require(std::isfinite(r) && r > previous, "Selected lens folds before the image edge");
    previous = r;
    if (r >= edge)
      break;
  }
  require(previous >= edge, "Selected lens does not cover the image");
  auto derivative = [&](double t) { return 1 - c[0] - c[1] - c[2] + t * (2 * c[2] + t * (3 * c[1] + t * 4 * c[0])); };
  require(derivative(0) > 0 && derivative(upper) > 0, "Selected lens radial derivative is not positive");
  const double a = 12 * c[0], b = 6 * c[1], d = 2 * c[2];
  auto check_extremum = [&](double t) {
    if (t > 0 && t < upper)
      require(derivative(t) > 0, "Selected lens has a folding radial branch");
  };
  if (a == 0) {
    if (b != 0)
      check_extremum(-d / b);
  } else if (b * b - 4 * a * d >= 0) {
    const double root = std::sqrt(b * b - 4 * a * d);
    check_extremum((-b - root) / (2 * a));
    check_extremum((-b + root) / (2 * a));
  }
  auto inverse_radius = [&](double r) {
    double lo = 0, hi = upper;
    for (int i = 0; i < 56; ++i) {
      const double mid = (lo + hi) / 2;
      if (radial(mid) < r)
        lo = mid;
      else
        hi = mid;
    }
    return (lo + hi) / 2;
  };
  const double lx = -double(width) / 2.0 - ox, rx = width / 2.0 - ox;
  const double lr = std::hypot(lx, oy), rr = std::hypot(rx, oy);
  const double lt = inverse_radius(lr / radius), rt = inverse_radius(rr / radius);
  auto angle = [&](double fov) {
    auto ray = [&](double x, double r, double t) {
      if (r == 0)
        return std::array<double, 3>{0, 0, 1};
      const double theta = lens.projection == "rectilinear" ? std::atan(t * 2 * radius / width * std::tan(fov / 2))
                                                            : t * fov * radius / width;
      return std::array<double, 3>{std::sin(theta) * x / r, -std::sin(theta) * oy / r, std::cos(theta)};
    };
    const auto l = ray(lx, lr, lt), r = ray(rx, rr, rt);
    return std::acos(std::clamp(l[0] * r[0] + l[1] * r[1] + l[2] * r[2], -1.0, 1.0));
  };
  if (to_physical)
    return angle(coverage * kPi / 180) * 180 / kPi;
  const double target = coverage * kPi / 180;
  double lo = 0, hi = 0, last = -1;
  const double limit = lens.projection == "rectilinear" ? kPi - 1e-6 : 2 * kPi - 1e-6;
  for (int i = 1; i <= 2048; ++i) {
    hi = limit * i / 2048;
    const double value = angle(hi);
    require(std::isfinite(value) && value > last, "FOV is outside the selected lens coverage");
    last = value;
    if (value >= target)
      break;
  }
  require(last >= target, "FOV is outside the selected lens coverage");
  for (int i = 0; i < 56; ++i) {
    const double mid = (lo + hi) / 2;
    if (angle(mid) < target)
      lo = mid;
    else
      hi = mid;
  }
  return (lo + hi) / 2 * 180 / kPi;
}
const StitchCameraConfiguration* matching_profile(
    const VideoLensMetadata& video,
    const std::vector<StitchCameraConfiguration>& profiles) {
  const StitchCameraConfiguration* match = nullptr;
  for (const auto& profile : profiles) {
    if (profile.metadata_match.empty())
      continue;
    bool compatible = true;
    for (const auto& [key, value] : profile.metadata_match) {
      const auto found = video.recording_mode.find(key);
      compatible &=
          found != video.recording_mode.end() && std::find(value.begin(), value.end(), found->second) != value.end();
    }
    // An explicit lens accessory must be qualified by the profile too.
    if (video.recording_mode.count("lens_id") && !profile.metadata_match.count("lens_id"))
      compatible = false;
    if (!compatible)
      continue;
    if (match)
      return nullptr; // Ambiguous defaults are not an automatic choice.
    match = &profile;
  }
  return match;
}
std::string recording_description(const VideoLensMetadata& video) {
  std::ostringstream description;
  description << (video.model.empty() ? "Camera model not recorded" : video.model);
  if (video.width && video.height)
    description << "; " << video.width << "×" << video.height;
  const auto fps = video.recording_mode.find("frame_rate");
  if (fps != video.recording_mode.end() && !fps->second.empty())
    description << "; " << fps->second << " fps";
  if (!video.mode.empty())
    description << "; mode: " << (video.mode == "W" ? "Wide" : video.mode);
  if (!video.stabilization.empty())
    description << "; stabilization: " << video.stabilization;
  const auto zoom = video.recording_mode.find("zoom");
  if (zoom != video.recording_mode.end())
    description << "; zoom: " << std::setprecision(6) << std::stod(zoom->second) << "x";
  description << std::fixed << std::setprecision(2);
  if (video.horizontal_fov)
    description << "; horizontal coverage: " << *video.horizontal_fov << "°";
  if (video.vertical_fov)
    description << "; vertical coverage: " << *video.vertical_fov << "°";
  return description.str();
}
} // namespace
AutoInputLensResult ResolveAutoInputLens(
    const std::vector<std::filesystem::path>& videos,
    const StitchCameraSelection& fallback,
    const std::vector<StitchCameraConfiguration>& profiles) {
  AutoInputLensResult result{fallback, ""};
  std::string profile_name = fallback.configuration;
  for (const auto& profile : profiles)
    if (profile.id == fallback.configuration)
      profile_name = profile.display_name;
  const std::string baseline_source = "Fallback defaults: " + profile_name + " (not from MP4)";
  const std::string manual_source = "Manual / selected profile (MP4 import off)";
  std::string horizontal_source = fallback.automatic_fov() ? baseline_source : manual_source;
  std::string vertical_source = horizontal_source;
  std::string fov_note;
  result.lens_message = fallback.automatic_lens() ? baseline_source : manual_source;
  result.recording_message = "Not checked: automatic camera settings are off.";
  auto finish = [&](const std::string& warning = "") {
    result.fov_message = "Horizontal: " + horizontal_source + "\nVertical: " + vertical_source;
    if (!fov_note.empty())
      result.fov_message += "\n" + fov_note;
    result.message = (warning.empty() ? "" : warning + "\n") + "Camera FOV: " + result.fov_message +
        ". Lens correction: " + result.lens_message + ".";
    if (!warning.empty()) {
      result.fov_message += "\n" + warning;
      result.lens_message += "\n" + warning;
    }
    return result;
  };
  if (!fallback.automatic_fov() && !fallback.automatic_lens())
    return finish();
  result.recording_message = "Not checked: no source videos selected.";
  if (videos.empty())
    return finish("No source videos selected");
  if (videos.size() > 256) {
    result.recording_message = "Not checked: too many chapters to inspect.";
    return finish("Too many chapters to inspect");
  }
  std::optional<VideoLensMetadata> first;
  std::string unavailable;
  size_t metadata_count = 0;
  std::vector<std::string> descriptions;
  std::string fov_unavailable, lens_unavailable;
  bool all_fov_h = true, all_fov_v = true, all_lens = true, same_lens = true;
  for (const auto& path : videos) {
    auto value = ReadVideoLensMetadata(path);
    if (!value.ok()) {
      if (unavailable.empty())
        unavailable = path.filename().string() + ": " + std::string(value.status().message());
      all_fov_h = all_fov_v = all_lens = false;
      continue;
    }
    ++metadata_count;
    const auto description = recording_description(*value);
    if (descriptions.size() < 3 &&
        std::find(descriptions.begin(), descriptions.end(), description) == descriptions.end())
      descriptions.push_back(description);
    if (fov_unavailable.empty() && !value->fov_unavailable_reason.empty())
      fov_unavailable = value->fov_unavailable_reason;
    if (lens_unavailable.empty() && !value->unavailable_reason.empty())
      lens_unavailable = value->unavailable_reason;
    all_fov_h &= value->horizontal_fov.has_value();
    all_fov_v &= value->vertical_fov.has_value();
    all_lens &= value->unavailable_reason.empty();
    if (!first)
      first = *value;
    else {
      result.mismatch |= first->width != value->width || first->height != value->height ||
          first->model != value->model || first->mode != value->mode || first->stabilization != value->stabilization ||
          first->recording_mode != value->recording_mode;
      if (first->horizontal_fov && value->horizontal_fov)
        result.mismatch |= !close(*first->horizontal_fov, *value->horizontal_fov);
      if (first->vertical_fov && value->vertical_fov)
        result.mismatch |= !close(*first->vertical_fov, *value->vertical_fov);
      if (first->unavailable_reason.empty() && value->unavailable_reason.empty())
        same_lens &= close(first->camera.horizontal_fov, value->camera.horizontal_fov) &&
            close(first->camera.vertical_fov, value->camera.vertical_fov) &&
            first->camera.input_lens == value->camera.input_lens;
    }
  }
  result.mismatch |= !same_lens;
  if (metadata_count == videos.size() && !result.mismatch) {
    result.recording_metadata_found = true;
    result.recording_message = "Read from MP4 in all " + std::to_string(videos.size()) + " selected files.";
  } else if (metadata_count) {
    result.recording_message = result.mismatch
        ? "MP4 metadata differs between cameras/chapters; automatic optics were not imported."
        : "Incomplete: MP4 metadata read in " + std::to_string(metadata_count) + " of " +
            std::to_string(videos.size()) + " selected files; automatic optics were not imported.";
  } else {
    result.recording_message = "No supported MP4 recording metadata could be read.";
  }
  for (const auto& description : descriptions)
    result.recording_message += "\n" + description;
  if (!unavailable.empty())
    result.recording_message += "\n" + unavailable;
  if (result.mismatch)
    return finish(
        "Warning: camera/chapter metadata differs; using shared baseline defaults. Match recording modes or select manual values");
  if (!unavailable.empty())
    return finish(unavailable);
  if (!first)
    return finish("No supported recording metadata");
  const auto* profile = matching_profile(*first, profiles);
  const std::string profile_source = profile ? "Mode profile: " + profile->display_name + " (not MP4 lens values)" : "";
  if (fallback.automatic_lens()) {
    if (all_lens) {
      result.camera.input_lens = first->camera.input_lens;
      result.lens_message =
          "Read from MP4: projection, distortion and center offsets below were converted from its lens model.";
      result.lens_from_mp4 = true;
      result.detected = true;
    } else if (profile) {
      result.camera.input_lens = profile->input_lens;
      result.lens_message = profile_source;
    } else if (!lens_unavailable.empty()) {
      result.lens_message += "\n" + lens_unavailable + "; no verified mode match.";
    }
  }
  if (fallback.automatic_fov()) {
    // Paired metadata/profile parameters must stay paired when available. An
    // independent physical angle is converted to the actually selected model.
    if (profile) {
      try {
        result.camera.horizontal_fov = result.camera.input_lens == profile->input_lens
            ? profile->horizontal_fov
            : hugin_fov(
                  hugin_fov(profile->horizontal_fov, first->width, first->height, profile->input_lens, true),
                  first->width,
                  first->height,
                  result.camera.input_lens);
        result.camera.vertical_fov = profile->vertical_fov;
        horizontal_source = vertical_source = profile_source;
      } catch (const std::exception& error) {
        horizontal_source += " (mode FOV cannot be combined: " + std::string(error.what()) + ")";
      }
    }
    if (all_fov_h) {
      try {
        result.camera.horizontal_fov = all_lens && fallback.automatic_lens()
            ? first->camera.horizontal_fov
            : hugin_fov(*first->horizontal_fov, first->width, first->height, result.camera.input_lens);
        std::ostringstream message;
        message << std::fixed << std::setprecision(2) << "Read from MP4: coverage " << *first->horizontal_fov << "°";
        if (std::abs(*first->horizontal_fov - result.camera.horizontal_fov) > 0.005)
          message << " (calibration FOV " << result.camera.horizontal_fov << "° with this correction)";
        horizontal_source = message.str();
        result.horizontal_fov_from_mp4 = true;
        result.detected = true;
      } catch (const std::exception& error) {
        horizontal_source += " (MP4 FOV cannot be combined: " + std::string(error.what()) + ")";
      }
    } else if (all_lens && fallback.automatic_lens()) {
      result.camera.horizontal_fov = first->camera.horizontal_fov;
      horizontal_source = "Derived from MP4 lens model (axis FOV not recorded separately)";
      result.horizontal_fov_from_mp4 = true;
    }
    if (all_fov_v) {
      result.camera.vertical_fov = *first->vertical_fov;
      std::ostringstream message;
      message << std::fixed << std::setprecision(2) << "Read from MP4: coverage " << *first->vertical_fov << "°";
      vertical_source = message.str();
      result.vertical_fov_from_mp4 = true;
      result.detected = true;
    }
    if (!all_fov_h || !all_fov_v)
      fov_note = fov_unavailable;
  }
  return finish();
}

AutoInputLensResult ResolveAutoInputLensForCameras(
    const std::vector<std::filesystem::path>& left,
    const std::vector<std::filesystem::path>& right,
    const StitchCameraSelection& fallback,
    const std::vector<StitchCameraConfiguration>& profiles) {
  auto videos = left;
  videos.insert(videos.end(), right.begin(), right.end());
  auto result = ResolveAutoInputLens(videos, fallback, profiles);
  if (!result.mismatch || left.empty() || right.empty() || videos.size() > 256)
    return result;

  auto automatic = fallback;
  automatic.auto_fov = automatic.auto_lens = true;
  const auto l = ResolveAutoInputLens(left, automatic, profiles);
  const auto r = ResolveAutoInputLens(right, automatic, profiles);
  const auto lm = ReadVideoLensMetadata(left.front()), rm = ReadVideoLensMetadata(right.front());
  auto describe = [](const char* role, const AutoInputLensResult& resolved, const auto& metadata) {
    std::ostringstream out;
    out << role << ": " << (metadata.ok() ? recording_description(*metadata) : "Metadata unavailable");
    if (resolved.lens_from_mp4)
      out << "\nCalibration FOV: " << std::setprecision(10) << resolved.camera.horizontal_fov << "°. "
          << "Distortion a/b/c: " << std::setprecision(6) << resolved.camera.input_lens.distortion[0] << ", "
          << resolved.camera.input_lens.distortion[1] << ", " << resolved.camera.input_lens.distortion[2]
          << ". Center X/Y: " << resolved.camera.input_lens.center_offset[0] * 100 << "%, "
          << resolved.camera.input_lens.center_offset[1] * 100 << "%.";
    if (resolved.mismatch)
      out << "\nChapters within this camera disagree; its values cannot be used for both cameras.";
    return out.str();
  };
  result.shared_choices_message = "The cameras have different recording metadata.\n" + describe("Left", l, lm) + "\n" +
      describe("Right", r, rm) +
      "\nChoose a shared approximation below, or keep the fallback. A choice copies FOV and lens values into manual settings for both cameras.";
  auto complete = [](const AutoInputLensResult& value) {
    return !value.mismatch && value.recording_metadata_found && value.horizontal_fov_from_mp4 &&
        value.vertical_fov_from_mp4 && value.lens_from_mp4;
  };
  auto manual = [](StitchCameraSelection camera) {
    camera.configuration = "manual";
    camera.auto_fov = camera.auto_lens = false;
    return camera;
  };
  if (complete(l))
    result.shared_choices["left"] = manual(l.camera);
  if (complete(r))
    result.shared_choices["right"] = manual(r.camera);
  if (complete(l) && complete(r) && lm.ok() && rm.ok() && lm->width == rm->width && lm->height == rm->height &&
      l.camera.input_lens.projection == r.camera.input_lens.projection &&
      l.camera.input_lens.optimization == r.camera.input_lens.optimization) {
    auto average = manual(l.camera);
    average.horizontal_fov = (l.camera.horizontal_fov + r.camera.horizontal_fov) / 2;
    average.vertical_fov = (l.camera.vertical_fov + r.camera.vertical_fov) / 2;
    for (size_t i = 0; i < average.input_lens.distortion.size(); ++i)
      average.input_lens.distortion[i] = (l.camera.input_lens.distortion[i] + r.camera.input_lens.distortion[i]) / 2;
    for (size_t i = 0; i < average.input_lens.center_offset.size(); ++i)
      average.input_lens.center_offset[i] =
          (l.camera.input_lens.center_offset[i] + r.camera.input_lens.center_offset[i]) / 2;
    try {
      // Averaging coefficients can introduce a folding radial branch. Check
      // the entire image before offering the approximation.
      hugin_fov(average.horizontal_fov, lm->width, lm->height, average.input_lens, true);
      result.shared_choices["average"] = average;
    } catch (const std::exception&) {
      result.shared_choices_message += "\nAverage unavailable: the combined lens model is not invertible.";
    }
  }
  result.message += "\n" + result.shared_choices_message;
  return result;
}
} // namespace hm::stitching
