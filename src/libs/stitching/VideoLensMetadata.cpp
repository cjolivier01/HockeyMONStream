#include "hstream/src/libs/stitching/VideoLensMetadata.h"

#include <sys/stat.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
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
  if (lens.empty())
    throw Unsupported("GoPro file has no FOVL lens prescription");
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
  const double ratio = double(result.width) / result.height;
  require(
      std::abs(number(lens, "ARUW") - ratio) < 1e-5 && std::abs(number(lens, "ARWA") - ratio) < 1e-5,
      "GoPro aspect ratio does not match recorded image");
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
    require(
        std::abs(number(lens, axis.first) - angle(axis.second) * 360 / kPi) < 0.25,
        "GoPro lens prescription disagrees with recorded field of view");
  result.camera = fit(result.width, result.height, angle);
  result.camera.vertical_fov = number(lens, "YFOV");
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
  result.camera = fit(result.width, result.height, inverse(radius, extent), extent);
  result.camera.input_lens.center_offset = {
      (v[4] - result.width / 2.0) / result.width, (v[5] - result.height / 2.0) / result.height};
  if (std::abs(result.camera.input_lens.center_offset[0]) > 0.01 ||
      std::abs(result.camera.input_lens.center_offset[1]) > 0.01)
    return unavailable(result, "optical center is outside the supported radial fit margin");
  result.mode = "raw v3";
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
  result.camera = fit(width, height, inverse(radius, std::hypot(double(width), double(height)) / 2));
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
  std::vector<Atom> atoms(uint64_t pos, uint64_t length) {
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
    if (!vendor->unavailable_reason.empty())
      return *vendor;
  }
  Mp4 mp4(path);
  size_t width = 0, height = 0;
  std::map<std::string, Mp4::Atom> dji_track;
  for (const auto& top : mp4.atoms(0, mp4.size)) {
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

AutoInputLensResult ResolveAutoInputLens(
    const std::vector<std::filesystem::path>& videos,
    const StitchCameraSelection& fallback) {
  AutoInputLensResult result{fallback, "Auto: no source videos selected; using automatic lens fitting."};
  if (videos.empty())
    return result;
  if (videos.size() > 256) {
    result.message = "Auto: too many chapters to inspect; using automatic lens fitting.";
    return result;
  }
  std::optional<VideoLensMetadata> first;
  std::string unavailable;
  for (const auto& path : videos) {
    auto value = ReadVideoLensMetadata(path);
    if (!value.ok()) {
      if (unavailable.empty())
        unavailable = path.filename().string() + ": " + std::string(value.status().message());
      continue;
    }
    if (!value->unavailable_reason.empty() && unavailable.empty())
      unavailable = path.filename().string() + ": " + value->unavailable_reason;
    if (!first)
      first = *value;
    else {
      auto close = [](double a, double b) { return std::abs(a - b) <= 1e-6; };
      bool same = first->width == value->width && first->height == value->height && first->model == value->model &&
          first->mode == value->mode && first->stabilization == value->stabilization &&
          first->unavailable_reason == value->unavailable_reason &&
          close(first->camera.horizontal_fov, value->camera.horizontal_fov) &&
          close(first->camera.vertical_fov, value->camera.vertical_fov);
      for (size_t i = 0; i < 3; ++i)
        same &= close(first->camera.input_lens.distortion[i], value->camera.input_lens.distortion[i]);
      for (size_t i = 0; i < 2; ++i)
        same &= close(first->camera.input_lens.center_offset[i], value->camera.input_lens.center_offset[i]);
      result.mismatch |= !same;
    }
  }
  if (result.mismatch)
    result.message =
        "Warning: camera/chapter lens metadata differs. One shared calibration is required; using automatic fitting. Match recording modes or choose a manual profile.";
  else if (!unavailable.empty())
    result.message = "Auto unavailable: " + unavailable + ". Using automatic lens fitting.";
  else if (first) {
    result.camera = first->camera;
    result.detected = true;
    result.message = "Auto: " + first->model + " / " + first->mode + " / " + first->stabilization +
        ". Shared lens values loaded from all source chapters.";
  }
  return result;
}
} // namespace hm::stitching
