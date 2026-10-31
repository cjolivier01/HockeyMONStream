#include "hstream/src/libs/common/ConfigLayers.h"
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
Bytes gopro(
    float zoom = 0.476107776165009f,
    const char* eis = "HS EIS",
    bool warp = false,
    bool prescription = true,
    bool fov = true) {
  Bytes global, lens;
  append(global, tag("MINF", 'c', chars("MISSION 1 PRO")));
  append(global, tag("PRJT", 'F', chars("GPRO"), 4));
  append(global, tag("EISA", 'c', chars(eis)));
  append(global, tag("HCTL", 'c', chars("Off")));
  append(global, tag("ZOOM", 'f', floats({1})));
  Bytes fps = be(60000);
  append(fps, be(1001));
  append(global, tag("VFPS", 'L', fps));
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
  if (fov) {
    append(lens, tag("XFOV", 'f', floats({120.000427246094f})));
    append(lens, tag("YFOV", 'f', floats({70.3657608032227f})));
    append(lens, tag("ZFOV", 'f', floats({135.511917114258f})));
  }
  append(lens, tag("ZMPL", 'f', floats({zoom})));
  if (prescription) {
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
  }
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
  if (argc == 3 && std::string(argv[1]) == "--projection-limits") {
    try {
      const auto cases = YAML::LoadFile(argv[2]);
      expect(cases.IsSequence() && cases.size() <= 10000, "Expected bounded projection cases");
      YAML::Node results(YAML::NodeType::Sequence);
      for (const auto& item : cases) {
        const auto projection = ParseStitchProjection(item["projection"].as<std::string>());
        expect(projection.ok(), projection.status().ToString().c_str());
        const auto maximum =
            MaximumStitchProjectionHorizontalFov(*projection, item["parameters"].as<std::vector<double>>());
        expect(maximum.ok(), maximum.status().ToString().c_str());
        results.push_back(*maximum);
      }
      std::cout << YAML::Dump(results) << '\n';
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  if (argc == 3 && std::string(argv[1]) == "--layering") {
    try {
      const auto fixture = YAML::LoadFile(argv[2]);
      const auto cases = fixture["cases"];
      expect(cases.IsSequence() && cases.size() <= 10000, "Expected bounded optics layering cases");
      YAML::Node results(YAML::NodeType::Sequence);
      for (const auto& item : cases) {
        auto effective = YAML::Clone(fixture["baseline"]);
        for (const auto& layer : item["layers"]) {
          effective = hm::config::MergeNodes(effective, layer, false);
          PreserveLegacyInputLensSelection(effective, layer);
        }
        const auto camera = read_stitch_camera_selection(effective);
        expect(camera.ok(), camera.status().ToString().c_str());
        YAML::Node result;
        result["id"] = item["id"].as<std::string>();
        result["fov"] = camera->automatic_fov();
        result["lens"] = camera->automatic_lens();
        write_stitch_camera_selection(result, *camera);
        results.push_back(result);
      }
      std::cout << YAML::Dump(results) << '\n';
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  if (argc == 4 && std::string(argv[1]) == "--camera-pair") {
    const auto result = ResolveAutoInputLensForCameras({argv[2]}, {argv[3]}, {"auto"});
    YAML::Node report;
    report["message"] = result.shared_choices_message;
    for (const auto& [key, camera] : result.shared_choices) {
      YAML::Node config;
      write_stitch_camera_selection(config, camera);
      report["choices"][key] = config["stitching"];
    }
    std::cout << report << '\n';
    return result.shared_choices.empty() ? 1 : 0;
  }
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
    expect(
        HasManualInputLensOverrides(YAML::Load("stitching: {input_lens: {distortion: [0, 0, 0]}}")),
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
    for (size_t length = 0; length < fixture.size(); ++length) {
      const auto partial = ParseVideoLensMetadata(Bytes(fixture.begin(), fixture.begin() + length));
      expect(!partial.ok() || !partial->unavailable_reason.empty(), "Truncation cannot yield a complete lens");
    }
    auto good = movie(fixture);
    write(dir / "left.mp4", good);
    write(dir / "right.mp4", good);
    write(dir / "zoomed.mp4", movie(gopro(0.36623674631118774f, "HS EIS", false, true, false)));
    const auto pair = ResolveAutoInputLensForCameras({dir / "left.mp4"}, {dir / "zoomed.mp4"}, {"auto"});
    expect(
        pair.mismatch && !pair.detected && pair.shared_choices.size() == 3,
        "Different supported camera optics need explicit choices, not an automatic import");
    expect(
        pair.shared_choices_message.find("Left:") != std::string::npos &&
            pair.shared_choices_message.find("Right:") != std::string::npos &&
            pair.shared_choices_message.find("125.122") != std::string::npos &&
            pair.shared_choices_message.find("97.077") != std::string::npos,
        "Mismatch explanation identifies both cameras and their differing calibration FOV");
    const auto& left_choice = pair.shared_choices.at("left");
    const auto& right_choice = pair.shared_choices.at("right");
    expect(
        left_choice.input_lens == ReadVideoLensMetadata(dir / "left.mp4")->camera.input_lens &&
            right_choice.input_lens == ReadVideoLensMetadata(dir / "zoomed.mp4")->camera.input_lens &&
            !left_choice.automatic_fov() && !left_choice.automatic_lens(),
        "Explicit camera choices copy the exact prescription and disable both automatic policies");
    const auto& average = pair.shared_choices.at("average");
    expect(
        average.horizontal_fov > right_choice.horizontal_fov && average.horizontal_fov < left_choice.horizontal_fov &&
            average.input_lens.distortion[1] > right_choice.input_lens.distortion[1] &&
            average.input_lens.distortion[1] < left_choice.input_lens.distortion[1],
        "The shared average must be intermediate in both FOV and lens correction");
    YAML::Node snapshot;
    write_stitch_camera_selection(snapshot, right_choice);
    const auto reopened = read_stitch_camera_selection(snapshot);
    expect(
        reopened.ok() && *reopened == right_choice && reopened->same_automation(right_choice) &&
            ResolveAutoInputLensForCameras({dir / "left.mp4"}, {dir / "zoomed.mp4"}, *reopened).camera == right_choice,
        "A saved manual choice survives reopening and cannot be replaced by automatic metadata");
    const auto inconsistent =
        ResolveAutoInputLensForCameras({dir / "left.mp4", dir / "zoomed.mp4"}, {dir / "zoomed.mp4"}, {"auto"});
    expect(
        inconsistent.shared_choices.size() == 1 && inconsistent.shared_choices.count("right"),
        "A camera with inconsistent chapters cannot supply a shared choice or average");
    const auto incomplete =
        ResolveAutoInputLensForCameras({dir / "left.mp4", dir / "missing.mp4"}, {dir / "zoomed.mp4"}, {"auto"});
    expect(
        !incomplete.shared_choices.count("left") && !incomplete.shared_choices.count("average"),
        "Incomplete camera metadata cannot supply a choice or average");
    std::filesystem::remove(dir / "zoomed.mp4");
    auto result = ResolveAutoInputLens({dir / "left.mp4", dir / "right.mp4"}, {"auto"});
    expect(result.detected && !result.mismatch, "Agreeing sources should import");
    expect(
        result.recording_metadata_found && result.horizontal_fov_from_mp4 && result.vertical_fov_from_mp4 &&
            result.lens_from_mp4 && result.recording_message.find("MISSION 1 PRO") != std::string::npos &&
            result.recording_message.find("7680×4320") != std::string::npos &&
            result.fov_message.find("120.00°") != std::string::npos &&
            result.fov_message.find("125.12°") != std::string::npos,
        "MP4 provenance must identify the recording and distinguish physical coverage from converted calibration values");
    YAML::Node config = YAML::Load(
        "stitching: {camera_config: auto, camera_configs: {auto: {display_name: Auto, horizontal_fov: 108, vertical_fov: 90}}}");
    auto selected = read_stitch_camera_selection(config);
    expect(selected.ok(), "Auto profile reads without filesystem work");
    auto game_result = ReadAutoInputLensForGame(dir, config, *selected);
    expect(game_result.detected, "Auto discovers both originals");
    const auto live = ReadAutoInputLensForGame(dir, config, *selected, false, true);
    expect(
        !live.detected && live.camera == *selected && live.message.find("live cameras") != std::string::npos,
        "Live sources must not import supported recordings from the same game directory");
    YAML::Node legacy = YAML::Load("stitching: {camera_config: null, camera_fov: {horizontal_fov: 117}}");
    auto effective_legacy = YAML::Clone(config);
    effective_legacy["stitching"]["camera_config"] = YAML::Node(YAML::NodeType::Null);
    effective_legacy["stitching"]["camera_configs"]["auto"]["vertical_fov"] = 75;
    PreserveLegacyInputLensSelection(effective_legacy, legacy);
    expect(
        effective_legacy["stitching"]["camera_config"].as<std::string>() == "manual" &&
            effective_legacy["stitching"]["camera_fov"]["vertical_fov"].as<double>() == 75,
        "A null camera selector preserves legacy manual intent and inherited optics");
    auto policy_override = YAML::Clone(config);
    policy_override["stitching"]["camera_automation"] = YAML::Load("{fov: false, lens: false}");
    const auto auto_layer = YAML::Load("stitching: {camera_config: auto, camera_automation: {lens: false}}");
    PreserveLegacyInputLensSelection(policy_override, auto_layer);
    const auto higher_camera = read_stitch_camera_selection(policy_override);
    expect(
        higher_camera.ok() && higher_camera->automatic_fov() && !higher_camera->automatic_lens(),
        "A higher-layer camera selector resets inherited policies while retaining explicit independent choices");
    config["stitching"]["auto_input_lens_frozen"] = true;
    auto frozen = ReadAutoInputLensForGame(dir, config, game_result.camera);
    expect(!frozen.detected && frozen.camera == game_result.camera, "Experiment optics remain frozen");
    expect(
        !frozen.recording_metadata_found && !frozen.horizontal_fov_from_mp4 && !frozen.vertical_fov_from_mp4 &&
            !frozen.lens_from_mp4 && frozen.recording_message.find("Not checked") != std::string::npos,
        "A saved snapshot must not claim to have inspected the current MP4s");
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
    result = ResolveAutoInputLens({dir / "left.mp4", dir / "right.mp4"}, {"auto"});
    expect(result.mismatch && !result.detected, "File replacement invalidates cache; different stabilization warns");
    expect(
        !result.recording_metadata_found && !result.horizontal_fov_from_mp4 && !result.vertical_fov_from_mp4 &&
            !result.lens_from_mp4 && result.recording_message.find("differs") != std::string::npos,
        "Mismatched recordings cannot appear as successfully imported shared optics");
    result = ResolveAutoInputLens({dir / "left.mp4", dir / "missing.mp4"}, {"auto"});
    expect(!result.detected, "Missing camera never copies the other lens");
    expect(
        !result.recording_metadata_found && !result.lens_from_mp4 &&
            result.recording_message.find("1 of 2") != std::string::npos,
        "Partial metadata reads must state how many files were read without claiming shared success");
    // FOV and correction are independent, including a valid FOV-only file.
    const auto fov_only = movie(gopro(0.476107776f, "HS EIS", false, false));
    write(dir / "left.mp4", fov_only);
    write(dir / "right.mp4", fov_only);
    result = ResolveAutoInputLens({dir / "left.mp4", dir / "right.mp4"}, *selected);
    expect(
        result.detected && std::abs(result.camera.horizontal_fov - 120.000427246094) < 1e-6 &&
            result.camera.input_lens == selected->input_lens,
        "Valid MP4 FOV survives missing lens coefficients; correction falls back independently");
    expect(
        result.recording_metadata_found && result.horizontal_fov_from_mp4 && result.vertical_fov_from_mp4 &&
            !result.lens_from_mp4 && result.lens_message.find("Fallback") != std::string::npos,
        "Recognized cameras with only FOV metadata must not highlight fallback lens values");
    StitchCameraConfiguration profile{
        "mode", "Verified mode", 125.12273264898, 70.3657608032, parsed->camera.input_lens};
    for (const auto& [key, value] : parsed->recording_mode)
      profile.metadata_match[key] = {value};
    result = ResolveAutoInputLens({dir / "left.mp4", dir / "right.mp4"}, *selected, {profile});
    expect(
        result.lens_message.find("Mode profile") != std::string::npos &&
            result.camera.input_lens == profile.input_lens && result.camera.horizontal_fov > 125,
        "Compatible mode correction combines with physical MP4 FOV using Hugin conversion");
    expect(
        !result.lens_from_mp4 && result.horizontal_fov_from_mp4 && result.vertical_fov_from_mp4,
        "Verified profile correction is distinct from imported MP4 FOV");
    auto incompatible = profile;
    incompatible.metadata_match["horizon"] = {"Locked"};
    result = ResolveAutoInputLens({dir / "left.mp4", dir / "right.mp4"}, *selected, {incompatible});
    expect(result.camera.input_lens == selected->input_lens, "A different mode cannot supply automatic correction");
    result = ResolveAutoInputLens({dir / "left.mp4", dir / "right.mp4"}, *selected, {profile, profile});
    expect(result.camera.input_lens == selected->input_lens, "Ambiguous mode profiles fall back");
    const auto lens_only = movie(gopro(0.476107776f, "HS EIS", false, true, false));
    write(dir / "left.mp4", lens_only);
    write(dir / "right.mp4", lens_only);
    for (bool automatic_lens : {false, true}) {
      auto choice = *selected;
      choice.auto_fov = true;
      choice.auto_lens = automatic_lens;
      result = ResolveAutoInputLens({dir / "left.mp4", dir / "right.mp4"}, choice);
      expect(
          result.detected && std::abs(result.camera.vertical_fov - 70.38137648094) < 1e-5 &&
              std::abs(result.camera.horizontal_fov - (automatic_lens ? 125.122732649 : 120.01437255148)) < 1e-5,
          "A complete lens model supplies both FOV axes without separate tags, independently of lens automation");
    }
    write(dir / "left.mp4", good);
    write(dir / "right.mp4", good);
    auto independent = *selected;
    independent.auto_fov = true;
    independent.auto_lens = false;
    config["hstream_ui"]["video_roles"]["right"][0] = "right.mp4";
    result = ReadAutoInputLensForGame(dir, config, independent, true);
    expect(
        result.camera.input_lens == independent.input_lens &&
            std::abs(result.camera.horizontal_fov - 120.000427246094) < 1e-6,
        "Automatic FOV does not enable automatic lens correction");
    // Reference values obtained with pano_trafo 2025.0.1: the angular distance
    // between the two horizontal pixel boundaries must remain XFOV, including
    // the nonzero vertical optical-center displacement.
    independent.input_lens.distortion = {0, 0.02, 0};
    independent.input_lens.center_offset = {-0.01, 0.14};
    for (const auto& [projection, expected] : std::vector<std::pair<std::string, double>>{
             {"rectilinear", 123.7986576803}, {"full-frame-fisheye", 125.8372124449}}) {
      independent.input_lens.projection = projection;
      result = ReadAutoInputLensForGame(dir, config, independent, true);
      expect(
          std::abs(result.camera.horizontal_fov - expected) < 1e-5,
          "Independent physical FOV conversion agrees with Hugin including center offsets");
    }
    independent.auto_fov = false;
    independent.auto_lens = true;
    independent.horizontal_fov = 111;
    config["hstream_ui"]["video_roles"]["right"][0] = "right.mp4";
    result = ReadAutoInputLensForGame(dir, config, independent, true);
    expect(
        result.camera.horizontal_fov == 111 && result.camera.input_lens == parsed->camera.input_lens,
        "Automatic lens correction retains manual FOV");
    YAML::Node roundtrip;
    write_stitch_camera_selection(roundtrip, independent);
    const auto restored = read_stitch_camera_selection(roundtrip);
    expect(
        restored.ok() && !restored->automatic_fov() && restored->automatic_lens(),
        "Independent source policies round trip");
    roundtrip["stitching"]["auto_input_lens_frozen"] = true;
    independent.configuration = "manual";
    result = ReadAutoInputLensForGame(dir, roundtrip, independent, true);
    expect(
        result.camera == independent && !result.detected,
        "Mixed automatic/manual experiments freeze under every selector");
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
    expect(
        dji_unknown.ok() && !dji_unknown->unavailable_reason.empty(),
        "Missing DJI stabilization status cannot authorize raw lens import");
    const auto dji_empty_off = ParseDjiLensMetadata(dji(false, true, true), 3840, 2160);
    expect(
        dji_empty_off.ok() && dji_empty_off->unavailable_reason.empty() && dji_empty_off->camera == dji_raw->camera,
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
    auto stabilized = raw;
    append(stabilized, scalar(42, 1));
    auto stabilized_movie = movie({}, 4000, 3000);
    append(stabilized_movie, make_trailer(stabilized));
    write(dir / "insta-stabilized-left.mp4", stabilized_movie);
    write(dir / "insta-stabilized-right.mp4", stabilized_movie);
    const auto recognized =
        ResolveAutoInputLens({dir / "insta-stabilized-left.mp4", dir / "insta-stabilized-right.mp4"}, *selected);
    expect(
        recognized.recording_metadata_found && !recognized.horizontal_fov_from_mp4 &&
            !recognized.vertical_fov_from_mp4 && !recognized.lens_from_mp4 &&
            recognized.recording_message.find("Insta360 raw") != std::string::npos &&
            recognized.fov_message.find("FlowState") != std::string::npos &&
            recognized.lens_message.find("FlowState") != std::string::npos,
        "Recognizing a camera must remain distinct from being able to use its stabilized optics");
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
    expect(
        different_modes.mismatch && !different_modes.detected,
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
