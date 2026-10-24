#include "hstream/src/libs/program/ProgramDetectorSettings.h"

#include <unistd.h>

#include <fstream>
#include <iostream>
#include <stdexcept>

#include "hstream/src/libs/assets/AssetManager.h"

namespace {
void Check(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Fixture {
  std::filesystem::path root;
  Fixture() {
    std::string path = (std::filesystem::temp_directory_path() / "hstream-detector-settings-XXXXXX").string();
    Check(mkdtemp(path.data()), "Cannot create fixture");
    root = path;
  }
  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};
} // namespace
int main(int argc, char** argv) {
  try {
    Check(argc == 2, "Expected inference YAML");
    Fixture fixture;
    const auto config_path = fixture.root / "infer.yaml", model_path = fixture.root / "model.onnx";
    // ONNX graph input: float32 images[batch,3,736,1984]. No tensor payloads.
    const char model[] =
        "\x08\x08\x3a\x29\x5a\x27\x0a\x06\x69\x6d\x61\x67\x65\x73\x12\x1d"
        "\x0a\x1b\x08\x01\x12\x17\x0a\x07\x12\x05\x62\x61\x74\x63\x68\x0a"
        "\x02\x08\x03\x0a\x03\x08\xe0\x05\x0a\x03\x08\xc0\x0f";
    {
      std::ofstream output(model_path, std::ios::binary);
      output.write(model, sizeof(model) - 1);
    }
    auto config = YAML::LoadFile(argv[1]);
    config.remove("pretrained-assets");
    config["property"]["onnx-file"] = "model.onnx";
    {
      std::ofstream output(config_path);
      output << config;
    }
    auto settings = hm::program::PrepareProgramDetector(config_path, config, {});
    Check(settings.ok(), settings.status().ToString());
    Check(
        settings->onnx == model_path && settings->input_name == "images" && settings->width == 1984 &&
            settings->height == 736 && settings->fp16 && settings->classes == 80 &&
            settings->maximum_detections == 300 && settings->maintain_aspect && settings->symmetric_padding,
        "Detector model/configuration contract changed");
    config["property"]["network-mode"] = 0;
    settings = hm::program::PrepareProgramDetector(config_path, config, {});
    Check(settings.ok() && !settings->fp16, "Explicit FP32 precision changed");
    Check(
        absl::IsCancelled(hm::program::PrepareProgramDetector(config_path, config, [] { return true; }).status()),
        "Cancelled preparation continued");
    // The resolver must use this snapshot, even if the original config names
    // a different path. A preexisting declared asset is verified, never trusted.
    const auto sha = hm::assets::AssetManager::Sha256(model_path);
    Check(sha.ok(), sha.status().ToString());
    YAML::Node asset;
    asset["property"] = "onnx-file";
    asset["url"] = "https://invalid.example/model.onnx";
    asset["sha256"] = *sha;
    config["pretrained-assets"].push_back(asset);
    {
      std::ofstream output(config_path);
      output << "property: {onnx-file: wrong.onnx}\n";
    }
    settings = hm::program::PrepareProgramDetector(config_path, config, {});
    Check(settings.ok(), settings.status().ToString());
    {
      std::ofstream output(config_path);
      output << "malformed: [\n";
    }
    settings = hm::program::PrepareProgramDetector(config_path, config, {});
    Check(settings.ok(), "Reopened malformed YAML instead of using the snapshot");
    std::filesystem::remove(config_path);
    settings = hm::program::PrepareProgramDetector(config_path, config, {});
    Check(settings.ok(), "Required the original YAML after capturing its snapshot");
    {
      std::ofstream output(config_path);
      output << config;
    }
    std::filesystem::create_directory(fixture.root / "launch");
    std::filesystem::create_symlink(config_path, fixture.root / "launch" / "infer.yaml");
    {
      std::ofstream output(fixture.root / "launch" / "model.onnx");
      output << "wrong model";
    }
    settings = hm::program::PrepareProgramDetector(fixture.root / "launch" / "infer.yaml", config, {});
    Check(
        settings.ok() && settings->onnx == model_path,
        "Symlinked YAML resolved its model and declarations from different directories");
    // A symlinked parent still resolves the declared target before verification.
    std::filesystem::create_directory_symlink(fixture.root, fixture.root / "alias");
    config["property"]["onnx-file"] = "alias/model.onnx";
    settings = hm::program::PrepareProgramDetector(config_path, config, {});
    Check(settings.ok() && settings->onnx == model_path, "Equivalent declared model target was not resolved");
    std::filesystem::create_directories(fixture.root / "other" / "child");
    std::filesystem::create_directory_symlink(fixture.root / "other" / "child", fixture.root / "nested-alias");
    {
      std::ofstream output(fixture.root / "other" / "model.onnx");
      output << "wrong model";
    }
    config["property"]["onnx-file"] = "nested-alias/../model.onnx";
    settings = hm::program::PrepareProgramDetector(config_path, config, {});
    Check(settings.ok() && settings->onnx == model_path, "Model and asset normalized parent components differently");
    config["pretrained-assets"][0]["sha256"] = std::string(64, '0');
    Check(!hm::program::PrepareProgramDetector(config_path, config, {}).ok(), "Unverified declared model accepted");
    config.remove("pretrained-assets");
    config["property"]["onnx-file"] = "${HSTREAM_TEST_ABSENT_MODEL_DIR}/model.onnx";
    unsetenv("HSTREAM_TEST_ABSENT_MODEL_DIR");
    Check(!hm::program::PrepareProgramDetector(config_path, config, {}).ok(), "Unset path variable was erased");
    config["property"]["onnx-file"] = "model.onnx";
    for (const auto* patch :
         {"property: {network-mode: 1}",
          "property: {interval: 1}",
          "property: {model-color-format: 1}",
          "property: {process-mode: 2}",
          "property: {net-scale-factor: .nan}",
          "property: {symmetric-padding: 2}",
          "property: {offsets: '1;2;3'}",
          "property: {mean-file: mean.ppm}",
          "property: {input-tensor-meta: 1}",
          "property: {network-input-order: 1}",
          "property: {infer-dims: '3;32;32'}",
          "property: {filter-out-class-ids: '0'}",
          "class-attrs-all: {roi-top-offset: 100}",
          "class-attrs-all: {roi-bottom-offset: 100}",
          "class-attrs-all: {post-cluster-threshold: 0.9}",
          "class-attrs-all: {detected-min-w: 10}",
          "class-attrs-0: {pre-cluster-threshold: 0.9}",
          "class-attrs-all: {topk: 0}",
          "class-attrs-all: {nms-iou-threshold: 2}",
          "hstream-prebuilt-precision: bf16"}) {
      auto invalid = YAML::Clone(config);
      const auto overlay = YAML::Load(patch);
      for (const auto& entry : overlay) {
        if (entry.second.IsMap()) {
          for (const auto& child : entry.second)
            invalid[entry.first.as<std::string>()][child.first.as<std::string>()] = YAML::Clone(child.second);
        } else {
          invalid[entry.first.as<std::string>()] = YAML::Clone(entry.second);
        }
      }
      Check(
          !hm::program::PrepareProgramDetector(config_path, invalid, {}).ok(),
          "Unsupported detector contract accepted");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
