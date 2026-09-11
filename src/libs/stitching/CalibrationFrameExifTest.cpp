#include "hstream/src/libs/stitching/CalibrationFrameExif.h"

#include <unistd.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>

#include "hstream/src/libs/common/Process.h"
#include "tools/cpp/runfiles/runfiles.h"
#include "yaml-cpp/yaml.h"

namespace {
bool expect(bool ok, const char* message) {
  if (!ok)
    std::cerr << message << '\n';
  return ok;
}
std::string read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void append_le(std::string& bytes, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i)
    bytes += static_cast<char>(value >> (8 * i));
}
void append_double(std::string& bytes, double value) {
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  append_le(bytes, bits, 8);
}
std::string atom(const std::string& type, const std::string& data) {
  std::string bytes;
  for (int shift = 24; shift >= 0; shift -= 8)
    bytes += static_cast<char>((data.size() + 8) >> shift);
  return bytes + type + data;
}
} // namespace

int main(int argc, char** argv) {
  using namespace hm::stitching;
  using namespace hm::stitching::frame_exif;
  // Optional real-recording smoke check: test_binary video png original_pts_seconds.
  if (argc == 4) {
    CalibrationFrameExifWriter writer;
    const auto status = writer.Write(argv[2], {argv[1], std::stod(argv[3])});
    std::cout << status << '\n';
    return status.ok() ? 0 : 1;
  }
  bool ok = true;
  const auto gopro = Parse(
      R"([{
    "GoPro:Main:Model": "HERO11 Black",
    "GoPro:Main:CameraSerialNumber": "camera-one",
    "GoPro:Main:WhiteBalance": "AUTO",
    "GoPro:Main:Sharpness": "MED",
    "GoPro:Main:ExposureCompensation": -0.5,
    "GoPro:Main:FirmwareVersion": "H22.01.02.30",
    "QuickTime:Main:CreateDate": "2026:09:04 23:59:59",
    "Track4:Doc1:SampleTime": 0,
    "Track4:Doc1:SampleDuration": 1,
    "Track4:Doc1:ExposureTimes": "0.01 0.02 0.03 0.04",
    "Track4:Doc1:ISOSpeeds": "100 200 400 800",
    "Track4:Doc1:GPSLatitude": -33.5,
    "Track4:Doc1:GPSLongitude": -70.25,
    "Track4:Doc1:GPSAltitude": -2.5,
    "Track4:Doc1:GPSMeasureMode": 3,
    "Track4:Doc1:GPSSpeed": 36,
    "Track4:Doc1:GPSTrack": 123.5,
    "Track4:Doc1:GPSTrackRef": "M",
    "Track4:Doc2:SampleTime": 1,
    "Track4:Doc2:SampleDuration": 1,
    "Track4:Doc2:ExposureTimes": "0.005 0.006",
    "Track4:Doc2:ISOSpeeds": "1600 3200"
  }])",
      std::nullopt);
  ok &= expect(gopro.ok(), "GoPro JSON must parse");
  if (gopro.ok()) {
    auto tags = ForFrame(*gopro, 0.75);
    ok &= expect(tags["ExposureTime"] == "0.04" && tags["ISO"] == "800", "Must choose the frame's in-packet values");
    ok &= expect(
        tags["Make"] == "GoPro" && tags["SerialNumber"] == "camera-one" && tags["Software"] == "H22.01.02.30",
        "Must map camera identity and firmware");
    ok &= expect(
        tags["Sharpness"] == "0" && tags["WhiteBalance"] == "0" && tags["ExposureCompensation"] == "-0.5",
        "Must translate vendor enums and signed exposure bias");
    ok &= expect(
        tags["GPSLatitudeRef"] == "S" && tags["GPSLongitudeRef"] == "W" && tags["GPSAltitudeRef"] == "1" &&
            tags["GPSSpeedRef"] == "K",
        "GPS signs and units must survive");
    ok &= expect(
        tags["GPSTrack"] == "123.5" && tags["GPSTrackRef"] == "M",
        "GPS course must preserve an explicit magnetic reference");
    tags = ForFrame(*gopro, 1);
    ok &= expect(
        tags["ExposureTime"] == "0.005" && tags["ISO"] == "1600" && !tags.count("GPSLatitude") &&
            !tags.count("GPSTrack"),
        "Sample boundaries must not retain stale exposure or GPS");
    ok &= expect(
        tags["DateTimeOriginal"] == "2026:09:05 00:00:00" && tags["OffsetTimeOriginal"] == "+00:00",
        "Capture time must include chapter PTS across day boundaries");
    tags = ForFrame(*gopro, 2.25);
    ok &= expect(!tags.count("ISO") && tags["SubSecTimeOriginal"] == "250000", "Do not extrapolate stale telemetry");
    ok &= expect(
        ForFrame(*gopro, -1).empty() && ForFrame(*gopro, std::numeric_limits<double>::quiet_NaN()).empty(),
        "Invalid timestamps must not select metadata");
  }
  for (int count : {30, 60}) {
    Metadata rational;
    rational.camera["Make"] = "GoPro";
    Tags arrays;
    for (int i = 0; i < count; ++i) {
      arrays["ISOSpeeds"] += std::to_string((i + 1) * 100) + " ";
      arrays["ExposureTimes"] += std::to_string((i + 1) * 0.0001) + " ";
    }
    for (double start : {0.0, 13 * 1.001}) {
      rational.samples = {{start, 1.001, arrays}};
      for (int i = 0; i < count; ++i) {
        const double pts = std::floor((start + 1.001 * i / count) * 1e9) / 1e9;
        const auto tags = ForFrame(rational, pts);
        ok &= expect(
            tags.at("ISO") == std::to_string((i + 1) * 100) &&
                std::abs(std::stod(tags.at("ExposureTime")) - (i + 1) * 0.0001) < 1e-10,
            "Nanosecond-rounded 29.97/59.94 fps PTS must select the matching exposure and ISO");
        if (i > 0)
          ok &= expect(
              ForFrame(rational, pts - 2e-6).at("ISO") == std::to_string(i * 100),
              "Times outside the rounding tolerance must still select the earlier sample");
      }
    }
  }
  const auto gps = Parse(
      R"([{
    "GoPro:Main:Model": "HERO13 Black",
    "Track4:Doc1:SampleTime": 0, "Track4:Doc1:SampleDuration": 1,
    "Track4:Doc1:GPSLatitude": 40, "Track4:Doc1:GPSLongitude": 50,
    "Track4:Doc1:GPSMeasureMode": 3,
    "Track4:Doc1-1:GPSLatitude": 41, "Track4:Doc1-1:GPSLongitude": 51,
    "Track4:Doc1-2:GPSLatitude": 42, "Track4:Doc1-2:GPSLongitude": 52
  }])",
      std::nullopt);
  ok &= expect(
      gps.ok() && ForFrame(*gps, 0.5).at("GPSLatitude") == "41" && ForFrame(*gps, 0.9).at("GPSLongitude") == "52",
      "GPS must use the fix for this part of the packet");
  const std::string insta_json = R"([{
    "Insta360:Main:Model": "Insta360 Ace Pro 2",
    "Insta360:Main:SerialNumber": "camera-two",
    "Track1:Main:VideoFrameRate": 25,
    "Insta360:Doc1:TimeCode": 985000,
    "Insta360:Doc2:TimeCode": 985040,
    "Insta360:Doc2:ExposureTime": 0.004,
    "Insta360:Doc3:TimeCode": 985080,
    "Insta360:Doc3:ExposureTime": 0.008
  }])";
  const auto insta = Parse(insta_json, Insta360Clock{985000, 0.001});
  ok &= expect(insta.ok(), "Insta360 metadata must parse");
  if (insta.ok()) {
    auto tags = ForFrame(*insta, 0.085);
    ok &= expect(
        tags["Make"] == "Insta360" && tags["SerialNumber"] == "camera-two" && tags["ExposureTime"] == "0.008",
        "Must align Insta360's raw clock to physical frame PTS");
  }
  const auto no_clock = Parse(insta_json, std::nullopt);
  ok &= expect(
      no_clock.ok() && !ForFrame(*no_clock, 0.085).count("ExposureTime"),
      "Must not guess the origin or units of an undocumented clock");
  const auto utc_gps = [](const std::string& created, const std::string& first = "2026:09:04 12:00:00.5Z") {
    return Parse(
        R"([{"Insta360:Main:Model":"Insta360 Ace Pro 2", "QuickTime:Main:CreateDate":")" + created +
            R"(", "Insta360:Doc1:GPSDateTime":")" + first + R"(",
        "Insta360:Doc1:GPSLatitude":40, "Insta360:Doc1:GPSLongitude":-70,
        "Insta360:Doc2:GPSDateTime":"2026:09:04 12:00:00.75Z",
        "Insta360:Doc2:GPSLatitude":41, "Insta360:Doc2:GPSLongitude":-71,
        "Insta360:Doc3:GPSDateTime":"2026:09:04 12:00:03.5Z",
        "Insta360:Doc3:GPSLatitude":42, "Insta360:Doc3:GPSLongitude":-72}])",
        std::nullopt);
  };
  const auto insta_gps = utc_gps("2026:09:04 12:00:00");
  ok &= expect(insta_gps.ok(), "Insta360 UTC GPS must parse without an exposure clock");
  if (insta_gps.ok()) {
    ok &= expect(!ForFrame(*insta_gps, 0.49).count("GPSLatitude"), "Do not use a future GPS fix");
    ok &= expect(
        ForFrame(*insta_gps, 0.5).at("GPSLatitude") == "40" && ForFrame(*insta_gps, 0.75).at("GPSLatitude") == "41" &&
            ForFrame(*insta_gps, 3.5).at("GPSLongitude") == "72",
        "UTC GPS fixes must align to chapter PTS, ending at the next fix");
    ok &= expect(
        !ForFrame(*insta_gps, 1.75).count("GPSLatitude") && !ForFrame(*insta_gps, 4.5).count("GPSLatitude"),
        "UTC GPS fixes must expire after one second, including gaps and the final fix");
  }
  for (const char* invalid :
       {"", "0000:00:00 00:00:00", "2026:02:30 12:00:00", "2026:09:04 12:00:00+02:00", "2026:09:04 12:00:00junk"}) {
    const auto missing_origin = utc_gps(invalid);
    ok &= expect(
        missing_origin.ok() && !ForFrame(*missing_origin, 0.5).count("GPSLatitude") &&
            !ForFrame(*missing_origin, 0.5).count("DateTimeOriginal"),
        "Missing, invalid or non-UTC chapter origins must not produce timed GPS or capture time");
  }
  for (const char* invalid :
       {"2026:09:04 12:00:00.5", "2026:09:04 12:00:00.Z", "2026:02:30 12:00:00.5Z", "2026:09:04 12:00:00.5+02:00"}) {
    const auto invalid_fix = utc_gps("2026:09:04 12:00:00", invalid);
    ok &= expect(
        invalid_fix.ok() && !ForFrame(*invalid_fix, 0.5).count("GPSLatitude"),
        "Invalid or unzoned GPS timestamps must be skipped");
  }
  const auto fractional_origin = utc_gps("2026:09:04 12:00:00.25Z");
  ok &= expect(
      fractional_origin.ok() && ForFrame(*fractional_origin, 0.25).at("GPSLatitude") == "40" &&
          ForFrame(*fractional_origin, 0.25).at("SubSecTimeOriginal") == "500000",
      "GPS selection and capture time must agree for fractional UTC origins");
  ok &= expect(!Parse("[broken", std::nullopt).ok(), "Malformed metadata must be rejected");
  const auto ordinary = Parse(R"([{"QuickTime:Main:Model":"Other camera"}])", std::nullopt);
  ok &= expect(ordinary.ok() && ForFrame(*ordinary, 0).empty(), "Ordinary videos must remain untouched");

  // Tiny Insta360 trailer with protobuf fields 24 (first-frame timestamp),
  // 62 (raw clock units) and an unknown length-delimited field to skip.
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("hstream-insta-clock-" + std::to_string(getpid()) + ".mp4");
  const unsigned char proto[] = {0xc0, 1, 0xc0, 0x84, 0x3d, 0xf0, 3, 1, 0x12, 2, 0x61, 0x62};
  std::string bytes(reinterpret_cast<const char*>(proto), sizeof(proto));
  bytes.append("\1\1", 2);
  bytes.append({char(sizeof(proto)), 0, 0, 0});
  const size_t footer = bytes.size();
  bytes.resize(footer + 72, 0);
  bytes[footer + 32] = static_cast<char>(bytes.size());
  bytes.replace(footer + 40, 32, "8db42d694ccc418790edff439fe026bf");
  std::ofstream(path, std::ios::binary).write(bytes.data(), bytes.size());
  auto clock = ReadInsta360Clock(path);
  ok &= expect(
      clock.ok() && clock->has_value() && (**clock).origin == 1000 && (**clock).scale == 0.001,
      "Must read Insta360 clock origin and units from its bounded trailer");
  // The same trailer inside an inst atom followed by another MP4 box.
  const std::string enclosed =
      atom("ftyp", std::string("mp42\0\0\0\0mp42isom", 16)) + atom("inst", bytes) + atom("free", std::string(100, ' '));
  std::ofstream(path, std::ios::binary).write(enclosed.data(), enclosed.size());
  clock = ReadInsta360Clock(path);
  ok &= expect(
      clock.ok() && clock->has_value() && (**clock).origin == 1000,
      "Must find an inst atom even when it is not the last MP4 box");
  bytes[sizeof(proto) + 2] = static_cast<char>(255);
  std::ofstream(path, std::ios::binary).write(bytes.data(), bytes.size());
  ok &= expect(!ReadInsta360Clock(path).ok(), "Invalid trailer record lengths must be rejected");
  // Exercise the actual pinned runtime, including EXIF tag names and a PNG
  // eXIf chunk. No camera footage, GPU or separately installed ExifTool needed.
  const auto perl = hm::findExecutable("perl", {"PATH"});
  const auto ffmpeg = hm::findExecutable("ffmpeg", {"PATH"});
  if (!perl || !ffmpeg) {
    std::cerr << "PNG integration test requires perl and ffmpeg on PATH\n";
    ok = false;
  } else {
    const auto command = [](const std::vector<std::string>& args, std::string* output) {
      return hm::run_command(args, "", {{"LC_ALL", "C"}}, [&](const std::string& error, const std::string& line) {
        if (output && !line.empty())
          *output += line + "\n";
        if (!error.empty())
          std::cerr << error << "\n";
      });
    };
    ok &= expect(
        command(
            {*ffmpeg,
             "-v",
             "error",
             "-y",
             "-f",
             "lavfi",
             "-i",
             "color=s=16x16:r=25",
             "-t",
             "1",
             "-c:v",
             "mpeg4",
             "-metadata",
             "creation_time=2026-09-04T12:00:00Z",
             path.string()},
            nullptr) == 0,
        "Must create a tiny MP4 fixture");
    std::string exposure;
    for (uint64_t time : {1000000, 1040000}) {
      append_le(exposure, time, 8);
      append_double(exposure, time == 1000000 ? 0.005 : 0.01);
    }
    append_le(exposure, 0x400, 2);
    append_le(exposure, 32, 4);
    // Actual 53-byte Insta360 GPS records use Unix seconds and milliseconds,
    // not SampleTime or the exposure clock. ExifTool must recover these fixes.
    std::string gps_records;
    for (int milliseconds : {25, 500}) {
      append_le(gps_records, 1788523200, 8); // 2026-09-04 12:00:00 UTC
      append_le(gps_records, milliseconds, 2);
      gps_records += 'A';
      append_double(gps_records, milliseconds == 25 ? 40 : 41);
      gps_records += 'N';
      append_double(gps_records, 70);
      gps_records += 'W';
      append_double(gps_records, 10); // m/s
      append_double(gps_records, 180); // track
      append_double(gps_records, -2.5); // altitude
    }
    append_le(gps_records, 0x700, 2);
    append_le(gps_records, 106, 4);
    std::string camera;
    for (const auto& [field, value] :
         {std::pair{0x0a, std::string("serial-test")},
          std::pair{0x12, std::string("Insta360 Ace Pro 2")},
          std::pair{0x1a, std::string("firmware-test")}}) {
      camera += char(field);
      camera += char(value.size());
      camera += value;
    }
    camera.append(reinterpret_cast<const char*>(proto), sizeof(proto) - 4);
    std::string trailer = exposure + gps_records + camera;
    append_le(trailer, 0x101, 2);
    append_le(trailer, camera.size(), 4);
    trailer += std::string(32, '\0');
    append_le(trailer, trailer.size() + 40, 4);
    append_le(trailer, 1, 4);
    trailer += "8db42d694ccc418790edff439fe026bf";
    const auto inst_atom = atom("inst", trailer);
    std::ofstream(path, std::ios::binary | std::ios::app).write(inst_atom.data(), inst_atom.size());
    const unsigned char png_bytes[] = {
        137, 80, 78, 71, 13, 10, 26,  10,  0,   0,   0,   13, 73, 72, 68, 82, 0,  0,  0,   1,   0,  0,   0,
        1,   8,  2,  0,  0,  0,  144, 119, 83,  222, 0,   0,  0,  12, 73, 68, 65, 84, 120, 156, 99, 104, 112,
        248, 15, 0,  3,  3,  1,  192, 254, 135, 91,  228, 0,  0,  0,  0,  73, 69, 78, 68,  174, 66, 96,  130};
    const auto png = path.parent_path() / ("-calibration image " + std::to_string(getpid()) + ".png");
    std::ofstream(png, std::ios::binary).write(reinterpret_cast<const char*>(png_bytes), sizeof(png_bytes));
    CalibrationFrameExifWriter writer;
    const auto status = writer.Write(png, {path, 0.05});
    ok &= expect(status.ok(), "Pinned ExifTool must annotate the PNG");
    if (!status.ok())
      std::cerr << status << "\n";
    const auto annotated = read(png);
    ok &= expect(
        annotated.find("eXIf") != std::string::npos &&
            annotated.find(std::string(reinterpret_cast<const char*>(png_bytes + 33), 24)) != std::string::npos,
        "PNG must contain native EXIF and preserve the exact IDAT chunk");
    std::string error;
    const std::unique_ptr<bazel::tools::cpp::runfiles::Runfiles> runfiles(
        bazel::tools::cpp::runfiles::Runfiles::Create(argv[0], &error));
    if (!runfiles) {
      std::cerr << error << "\n";
      ok = false;
    } else {
      std::string json;
      ok &= expect(
          command(
              {*perl,
               runfiles->Rlocation("exiftool/exiftool"),
               "-config",
               "",
               "-j",
               "-G1",
               "-n",
               "-EXIF:all",
               "--",
               png.string()},
              &json) == 0,
          "PNG EXIF must be independently readable");
      try {
        const auto tags = YAML::Load(json)[0];
        ok &= expect(
            tags["IFD0:Make"].as<std::string>() == "Insta360" &&
                tags["ExifIFD:SerialNumber"].as<std::string>() == "serial-test" &&
                std::abs(tags["ExifIFD:ExposureTime"].as<double>() - 0.01) < 1e-8,
            "Written EXIF must contain camera serial and the selected frame exposure");
        ok &= expect(
            tags["GPS:GPSLatitude"].as<double>() == 40 && tags["GPS:GPSLongitude"].as<double>() == 70 &&
                tags["GPS:GPSLongitudeRef"].as<std::string>() == "W" && tags["GPS:GPSSpeed"].as<double>() == 36 &&
                tags["GPS:GPSTrack"].as<double>() == 180 && tags["GPS:GPSTrackRef"].as<std::string>() == "T" &&
                tags["GPS:GPSAltitudeRef"].as<int>() == 1 &&
                tags["GPS:GPSDateStamp"].as<std::string>() == "2026:09:04" &&
                tags["GPS:GPSTimeStamp"].as<std::string>() == "12:00:00.025",
            "Written EXIF must recover the UTC GPS record with fractional timestamp, signs, speed units and course");
      } catch (const YAML::Exception& e) {
        std::cerr << e.what() << "\n";
        ok = false;
      }
    }
    // All sampled frames from a chapter reuse one metadata read.
    std::filesystem::remove(path);
    ok &= expect(writer.Write(png, {path, 0.01}).ok(), "Further frame writes must use the chapter metadata cache");
    const auto preserved = read(png);
    CalibrationFrameExifWriter missing_source;
    ok &= expect(
        !missing_source.Write(png, {path, 0.01}).ok() && read(png) == preserved,
        "Unavailable source metadata must preserve the already saved PNG");
    CalibrationFrameExifWriter cancelled([] { return true; });
    ok &= expect(absl::IsCancelled(cancelled.Write(png, {path, 0.01})), "Cancellation must remain observable");
    std::filesystem::remove(png);
  }
  std::filesystem::remove(path);
  return ok ? 0 : 1;
}
