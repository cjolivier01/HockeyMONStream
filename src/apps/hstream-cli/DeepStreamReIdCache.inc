// Included inside PlayerModelCache.cpp's private namespace so the SDK model
// shares its cancellation, child supervision, locks and atomic cache publisher.

YAML::Node DeepStreamDocument(const fs::path& path) {
  Require(
      fs::is_regular_file(path) && fs::file_size(path) <= kMaximumDocument,
      "DeepStream tracker configuration is missing or oversized: " + path.string());
  std::ifstream input(path, std::ios::binary);
  std::string text(kMaximumDocument + 1, '\0');
  input.read(text.data(), text.size());
  text.resize(input.gcount());
  Require(!text.empty() && text.size() <= kMaximumDocument, "Cannot read DeepStream tracker configuration");
  if (text.rfind("%YAML:1.0", 0) == 0)
    text.erase(0, text.find('\n') == std::string::npos ? text.size() : text.find('\n') + 1);
  // Some NVIDIA releases put SPDX comments before the OpenCV YAML directive.
  const auto directive = text.find("\n%YAML:1.0");
  if (directive != std::string::npos)
    text.erase(directive + 1, 9);
  const auto node = YAML::Load(text);
  Require(node.IsMap(), "DeepStream tracker configuration must be a mapping");
  return node;
}

fs::path PrepareDeepStream(
    const YAML::Node& tracker,
    const fs::path& config_directory,
    int gpu,
    const YAML::Node& identity,
    const fs::path& builder,
    const fs::path& root,
    const PlayerModelCacheOptions& options) {
  constexpr const char* kModelName = "resnet50_market1501.etlt";
  constexpr const char* kModelSha = "0e5b7f702ce7e3734e45f27b819866f15f6b083048481d1afae2089136e20201";
  constexpr const char* kModelUrl =
      "https://api.ngc.nvidia.com/v2/models/org/nvidia/team/tao/reidentificationnet/deployable_v1.0/"
      "files?redirect=true&path=resnet50_market1501.etlt";
  const auto absolute = [&config_directory](const std::string& value) {
    fs::path path(value);
    return fs::canonical(path.is_absolute() ? path : config_directory / path);
  };
  const auto library = absolute(Scalar(tracker["ll-lib-file"]));
  const auto sdk = library.parent_path().parent_path();
  const auto sample_path = sdk / "samples/configs/deepstream-app/config_tracker_NvDCF_accuracy.yml";
  const auto sample = DeepStreamDocument(sample_path);
  auto reid = YAML::Clone(sample["ReID"]);
  Require(
      reid.IsMap() && fs::path(Scalar(reid["tltEncodedModel"])).filename() == kModelName,
      "This DeepStream installation does not provide the supported default tracker ReID profile");
  Require(
      Scalar(reid["tltModelKey"]) == "nvidia_tao" && reid["networkMode"].as<int>() == 1 &&
          reid["reidFeatureSize"].as<int>() == 256 && reid["keepAspc"].as<int>() == 1,
      "The DeepStream default ReID model profile has changed");
  const auto dimensions = reid["inferDims"];
  Require(
      dimensions.IsSequence() && dimensions.size() == 3 && dimensions[0].as<int>() == 3 &&
          dimensions[1].as<int>() == 256 && dimensions[2].as<int>() == 128,
      "The DeepStream default ReID model input dimensions have changed");
  // Keep NVIDIA's actual model and preprocessing. Bound engine and gallery
  // allocations to this application's documented resource limits.
  reid["batchSize"] = 32;
  reid["workspaceSize"] = 256;
  reid["reidHistorySize"] = 32;
  reid["reidType"] = 2;
  reid["outputReidTensor"] = 0;
  reid["modelEngineFile"] = "model.engine";
  reid.remove("tltEncodedModel");
  reid.remove("tltModelKey");
  reid.remove("onnxFile");
  std::string base_path = Scalar(tracker["ll-config-file"]);
  if (!base_path.empty() && base_path.back() == ';')
    base_path.pop_back();
  Require(
      !base_path.empty() && base_path.find(';') == std::string::npos,
      "DeepStream default ReID requires one native tracker configuration");
  const auto base = DeepStreamDocument(absolute(base_path));
  Require(
      base["VisualTracker"].IsMap() && base["VisualTracker"]["visualTrackerType"] &&
          (base["VisualTracker"]["visualTrackerType"].as<int>() == 1 ||
           base["VisualTracker"]["visualTrackerType"].as<int>() == 2),
      "DeepStream default ReID requires an NvDCF tracker configuration");
  YAML::Node contract;
  contract["schema_version"] = 1;
  contract["feature"] = "reid";
  contract["model_id"] = "deepstream";
  contract["source"]["file"] = "model.etlt";
  contract["source"]["sha256"] = kModelSha;
  contract["source"]["url"] = kModelUrl;
  contract["source"]["license"] =
      "NVIDIA model terms; https://catalog.ngc.nvidia.com/orgs/nvidia/teams/tao/models/reidentificationnet";
  contract["tracker"]["ReID"] = reid;
  contract["tracker"]["TrajectoryManagement"]["enableReAssoc"] = 1;
  contract["tracker"]["TrajectoryManagement"]["reidExtractionInterval"] = 8;
  contract["tracker_base"] = YAML::Clone(base);
  contract["tracker_library"] = library.string();
  // SDK upgrades must not reuse a cache entry prepared by another native tracker.
  contract["tracker_library_sha256"] = DigestFile(library, kMaximumEngine, options);
  const auto serialized = Emit(contract);
  const pa::CatalogModel model{
      "reid", "deepstream", "DeepStream tracker default", kModelUrl, kModelSha, serialized.c_str(), 32};
  return Prepare(model, gpu, identity, builder, root, options, sdk / "samples/models/Tracker" / kModelName);
}
