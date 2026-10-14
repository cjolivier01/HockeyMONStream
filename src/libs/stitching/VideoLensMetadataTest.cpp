#include "hstream/src/libs/stitching/AutoInputLens.h"

#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>

using namespace hm::stitching;
namespace {
using Bytes = std::vector<unsigned char>;
void expect(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void append(Bytes& a, const Bytes& b) {
  a.insert(a.end(), b.begin(), b.end());
}
Bytes be(uint64_t value, size_t n = 4) {
  Bytes result(n);
  for (size_t i = 0; i < n; ++i)
    result[n - i - 1] = value >> (8 * i);
  return result;
}
Bytes floats(std::initializer_list<float> values) {
  Bytes b;
  for (float v : values) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    append(b, be(bits));
  }
  return b;
}
Bytes tag(const char* key, char type, const Bytes& b, unsigned width = 0) {
  if (!width)
    width = type == 'f' || type == 'L' ? 4 : 1;
  Bytes result(key, key + 4);
  result.push_back(type);
  result.push_back(width);
  append(result, be(b.size() / width, 2));
  append(result, b);
  while (result.size() % 4)
    result.push_back(0);
  return result;
}
Bytes chars(const std::string& s) {
  return Bytes(s.begin(), s.end());
}
Bytes gopro(float zoom = 0.476107776165009f, const char* eis = "HS EIS", bool warp = false) {
  Bytes global, lens;
  append(global, tag("MINF", 'c', chars("MISSION 1 PRO")));
  append(global, tag("PRJT", 'F', chars("GPRO"), 4));
  append(global, tag("EISA", 'c', chars(eis)));
  Bytes res = be(7680);
  append(res, be(4320));
  append(global, tag("VRES", 'L', res));
  append(lens, tag("DVID", 'F', chars("FOVL"), 4));
  append(lens, tag("VFOV", 'c', chars("W")));
  append(lens, tag("ABSC", 'f', floats({0})));
  append(lens, tag("MAPX", 'f', floats({warp ? 2.f : 1.f})));
  append(lens, tag("MAPY", 'f', floats({1})));
  append(lens, tag("ARUW", 'f', floats({1.77777779102325f})));
  append(lens, tag("ARWA", 'f', floats({1.77777779102325f})));
  append(lens, tag("XFOV", 'f', floats({120.000427246094f})));
  append(lens, tag("YFOV", 'f', floats({70.3657608032227f})));
  append(lens, tag("ZFOV", 'f', floats({135.511917114258f})));
  append(lens, tag("ZMPL", 'f', floats({zoom})));
  append(
      lens,
      tag("POLY",
          'f',
          floats(
              {0,
               2.67053532600403f,
               0.22922171652317f,
               -2.08499050140381f,
               1.64104437828064f,
               -7.24889386797406e-13f,
               4.47891317133589e-13f,
               0}),
          32));
  Bytes names(64);
  for (size_t i = 0; i < 8; ++i) {
    names[8 * i] = 'r';
    names[8 * i + 1] = '0' + i;
  }
  append(lens, tag("PYCF", 'c', names, 8));
  Bytes result = tag("DEVC", 0, global);
  append(result, tag("DEVC", 0, lens));
  return result;
}
Bytes atom(const char* key, const Bytes& b) {
  Bytes v = be(b.size() + 8);
  append(v, chars(key));
  append(v, b);
  return v;
}
Bytes movie(const Bytes& gpmf, size_t width = 7680, size_t height = 4320) {
  Bytes stsd(100);
  const auto codec = chars("hvc1");
  std::copy(codec.begin(), codec.end(), stsd.begin() + 12);
  auto w = be(width, 2), h = be(height, 2);
  std::copy(w.begin(), w.end(), stsd.begin() + 40);
  std::copy(h.begin(), h.end(), stsd.begin() + 42);
  Bytes moov = gpmf.empty() ? Bytes{} : atom("udta", atom("GPMF", gpmf));
  append(moov, atom("trak", atom("mdia", atom("minf", atom("stbl", atom("stsd", stsd))))));
  return atom("moov", moov);
}
void write(const std::filesystem::path& path, const Bytes& data) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(data.data()), data.size());
}
Bytes varint(uint64_t value) {
  Bytes b;
  do {
    auto c = value & 127;
    value >>= 7;
    b.push_back(c | (value ? 128 : 0));
  } while (value);
  return b;
}
Bytes field(unsigned key, const Bytes& b) {
  Bytes v = varint((key << 3) | 2);
  append(v, varint(b.size()));
  append(v, b);
  return v;
}
Bytes scalar(unsigned key, uint64_t value) {
  Bytes b = varint(key << 3);
  append(b, varint(value));
  return b;
}
Bytes dji(bool eis, bool include_eis = true, bool empty_eis = false) {
  Bytes header = field(1, chars("dvtm_ac204.proto"));
  append(header, field(10, chars("DJI OsmoAction5 Pro")));
  Bytes clip = field(1, header);
  if (include_eis)
    append(clip, field(9, empty_eis ? Bytes{} : scalar(1, eis)));
  Bytes f = floats({3000});
  std::reverse(f.begin(), f.end());
  f.insert(f.begin(), 13);
  append(clip, field(8, f));
  append(clip, field(3, field(1, Bytes(16))));
  return field(1, clip);
}
} // namespace
int main(int argc, char** argv) {
  if (argc > 1) {
    for (int i = 1; i < argc; ++i) {
      const auto result = ReadVideoLensMetadata(argv[i]);
      std::cout << argv[i] << ": ";
      if (!result.ok())
        std::cout << result.status() << '\n';
      else {
        std::cout.precision(17);
        std::cout << result->model << " " << result->unavailable_reason << " H=" << result->camera.horizontal_fov
                  << " V=" << result->camera.vertical_fov << ' ' << SerializeStitchInputLens(result->camera.input_lens)
                  << '\n';
      }
    }
    return 0;
  }
  const auto dir = std::filesystem::temp_directory_path() / ("video-lens-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  try {
    for (const char* inherited : {"null", "{}", "{projection: null, distortion: null, center_offset: null}"}) {
      YAML::Node empty;
      empty["stitching"]["input_lens"] = YAML::Load(inherited);
      empty["stitching"]["camera_fov"] = YAML::Load("{horizontal_fov: null}");
      expect(!HasManualInputLensOverrides(empty), "Inherit-only optics must not disable Auto");
      empty["stitching"]["camera_fov"]["horizontal_fov"] = 117;
      expect(HasManualInputLensOverrides(empty), "An explicit FOV must preserve legacy manual intent");
    }
    expect(HasManualInputLensOverrides(YAML::Load("stitching: {input_lens: {distortion: [0, 0, 0]}}")),
           "Even explicit neutral distortion preserves manual intent");
    auto parsed = ParseVideoLensMetadata(gopro());
    expect(parsed.ok(), parsed.status().ToString().c_str());
    expect(std::abs(parsed->camera.horizontal_fov - 125.12273264898) < 1e-7, "GoPro conversion HFOV");
    expect(std::abs(parsed->camera.input_lens.distortion[1] - 0.04057437656) < 1e-8, "GoPro conversion b");
    expect(!ParseVideoLensMetadata(gopro(0.5)).ok(), "FOV inconsistency must reject");
    expect(
        !ParseVideoLensMetadata(gopro(0.476107776f, "AutoBoost"))->unavailable_reason.empty(),
        "AutoBoost must reject even at zero ABSC");
    expect(
        !ParseVideoLensMetadata(gopro(0.476107776f, "HS EIS", true))->unavailable_reason.empty(),
        "Digital warp must reject");
    expect(
        !ParseVideoLensMetadata(gopro(std::numeric_limits<float>::quiet_NaN())).ok(), "Nonfinite values must reject");
    const auto fixture = gopro();
    auto padded = fixture;
    padded.resize(fixture.size() + 23804, 0);
    expect(ParseVideoLensMetadata(padded).ok(), "GoPro chapters may end in zero padding, including a partial header");
    padded.back() = 1;
    expect(!ParseVideoLensMetadata(padded).ok(), "Nonzero partial trailing headers must reject");
    for (size_t length = 0; length < fixture.size(); ++length)
      expect(!ParseVideoLensMetadata(Bytes(fixture.begin(), fixture.begin() + length)).ok(), "Truncation must reject");
    auto good = movie(fixture);
    write(dir / "left.mp4", good);
    write(dir / "right.mp4", good);
    auto result = ResolveAutoInputLens({dir / "left.mp4", dir / "right.mp4"}, {});
    expect(result.detected && !result.mismatch, "Agreeing sources should import");
    YAML::Node config = YAML::Load(
        "stitching: {camera_config: auto, camera_configs: {auto: {display_name: Auto, horizontal_fov: 108, vertical_fov: 90}}}");
    auto selected = read_stitch_camera_selection(config);
    expect(selected.ok(), "Auto profile reads without filesystem work");
    auto game_result = ReadAutoInputLensForGame(dir, config, *selected);
    expect(game_result.detected, "Auto discovers both originals");
    const auto live = ReadAutoInputLensForGame(dir, config, *selected, false, true);
    expect(!live.detected && live.camera == *selected && live.message.find("live cameras") != std::string::npos,
           "Live sources must not import supported recordings from the same game directory");
    YAML::Node legacy = YAML::Load("stitching: {camera_config: null, camera_fov: {horizontal_fov: 117}}");
    auto effective_legacy = YAML::Clone(config);
    effective_legacy["stitching"]["camera_config"] = YAML::Node(YAML::NodeType::Null);
    effective_legacy["stitching"]["camera_configs"]["auto"]["vertical_fov"] = 75;
    PreserveLegacyInputLensSelection(effective_legacy, legacy);
    expect(effective_legacy["stitching"]["camera_config"].as<std::string>() == "manual" &&
               effective_legacy["stitching"]["camera_fov"]["vertical_fov"].as<double>() == 75,
           "A null camera selector preserves legacy manual intent and inherited optics");
    config["stitching"]["auto_input_lens_frozen"] = true;
    auto frozen = ReadAutoInputLensForGame(dir, config, game_result.camera);
    expect(!frozen.detected && frozen.camera == game_result.camera, "Experiment optics remain frozen");
    config["stitching"].remove("auto_input_lens_frozen");
    auto manual = game_result.camera;
    manual.configuration = "gopro-mission-1";
    expect(ReadAutoInputLensForGame(dir, config, manual).camera == manual, "Manual profile is never imported");
    config["hstream_ui"]["video_roles"]["left"].push_back("left.mp4");
    config["hstream_ui"]["video_roles"]["right"].push_back("missing.mp4");
    expect(!ReadAutoInputLensForGame(dir, config, *selected).detected, "Explicit selected sources take precedence");
    write(dir / "resized.mp4", movie(fixture, 3840));
    expect(
        !ReadVideoLensMetadata(dir / "resized.mp4").ok(), "Remuxed dimensions cannot reuse the original prescription");
    write(dir / "right.mp4", movie(gopro(0.476107776f, "Off")));
    result = ResolveAutoInputLens({dir / "left.mp4", dir / "right.mp4"}, {});
    expect(result.mismatch && !result.detected, "File replacement invalidates cache; different stabilization warns");
    result = ResolveAutoInputLens({dir / "left.mp4", dir / "missing.mp4"}, {});
    expect(!result.detected, "Missing camera never copies the other lens");
    // An extended-size, sparse 5 GiB mdat must be skipped with a seek.
    const auto huge = dir / "huge.mp4";
    {
      std::ofstream out(huge, std::ios::binary);
      Bytes header = be(1);
      append(header, chars("mdat"));
      append(header, be(uint64_t(5) << 30, 8));
      out.write(reinterpret_cast<const char*>(header.data()), header.size());
      out.seekp(uint64_t(5) << 30);
      out.write(reinterpret_cast<const char*>(good.data()), good.size());
    }
    expect(ReadVideoLensMetadata(huge).ok(), "Seek past huge mdat");
    const auto dji_raw = ParseDjiLensMetadata(dji(false), 3840, 2160);
    expect(dji_raw.ok() && dji_raw->unavailable_reason.empty(), "DJI raw OpenCV prescription");
    const auto dji_unknown = ParseDjiLensMetadata(dji(false, false), 3840, 2160);
    expect(dji_unknown.ok() && !dji_unknown->unavailable_reason.empty(),
           "Missing DJI stabilization status cannot authorize raw lens import");
    const auto dji_empty_off = ParseDjiLensMetadata(dji(false, true, true), 3840, 2160);
    expect(dji_empty_off.ok() && dji_empty_off->unavailable_reason.empty() && dji_empty_off->camera == dji_raw->camera,
           "A present empty DJI EIS message legitimately represents proto3 OFF");
    expect(!ParseDjiLensMetadata(dji(true), 3840, 2160)->unavailable_reason.empty(), "DJI EIS is not a raw lens");
    Bytes insta = field(2, chars("Insta360 Ace Pro 2"));
    append(insta, scalar(42, 1));
    auto unsupported = ParseInsta360LensMetadata(insta);
    expect(
        unsupported.ok() && unsupported->unavailable_reason.find("FlowState") != std::string::npos,
        "Insta360 baked stabilization explained");
    Bytes raw = field(2, chars("Insta360 raw")), dims = scalar(1, 4000);
    append(dims, scalar(2, 3000));
    append(raw, field(19, dims));
    append(raw, field(54, chars("1_0_10000_10000_2000_1500_0_0_0_0_0_0_0_0_0_0_0_4000_3000_126_197632")));
    const auto insta_raw = ParseInsta360LensMetadata(raw);
    expect(insta_raw.ok(), insta_raw.status().ToString().c_str());
    expect(insta_raw->unavailable_reason.empty(), "Raw Insta360 prescription must be supported");
    const auto make_trailer = [&](const Bytes& metadata) {
      Bytes trailer = metadata;
      auto append_le = [&](uint64_t value, size_t n) {
        auto b = be(value, n);
        std::reverse(b.begin(), b.end());
        append(trailer, b);
      };
      append_le(0x101, 2);
      append_le(metadata.size(), 4);
      trailer.resize(trailer.size() + 32);
      append_le(trailer.size() + 40, 4);
      append_le(1, 4);
      append(trailer, chars("8db42d694ccc418790edff439fe026bf"));
      return trailer;
    };
    const auto trailer = make_trailer(raw);
    for (bool boxed : {false, true}) {
      auto original = movie({}, 4000, 3000);
      append(original, boxed ? atom("inst", trailer) : trailer);
      const auto path = dir / (boxed ? "insta-boxed.mp4" : "insta-trailer.mp4");
      write(path, original);
      const auto read = ReadVideoLensMetadata(path);
      expect(read.ok() && read->camera == insta_raw->camera, "Read both raw and boxed Insta360 trailers");
    }
    for (int fov : {1, 3}) {
      auto mode = raw;
      append(mode, scalar(47, fov));
      auto original = movie({}, 4000, 3000);
      append(original, make_trailer(mode));
      write(dir / ("insta-fov-" + std::to_string(fov) + ".mp4"), original);
    }
    const auto different_modes = ResolveAutoInputLens({dir / "insta-fov-1.mp4", dir / "insta-fov-3.mp4"}, *selected);
    expect(different_modes.mismatch && !different_modes.detected,
           "Different Insta360 FOV modes must warn even when their raw lens coefficients match");
    std::filesystem::remove_all(dir);
    std::cout << "Video lens metadata tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    std::filesystem::remove_all(dir);
    return 1;
  }
}
