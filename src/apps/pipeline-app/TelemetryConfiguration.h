#pragma once

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

// Preserve text configs referenced by the resolved YAML. Directory-valued
// plugin configs (stitching artifacts) remain references; masks live in the DB.
// Missing/disabled inputs are explicit entries rather than silently omitted.
inline absl::StatusOr<YAML::Node> SnapshotTelemetryConfigFiles(
    const YAML::Node& config,
    const std::vector<std::filesystem::path>& roots) {
  namespace fs = std::filesystem;
  YAML::Node files(YAML::NodeType::Sequence);
  std::set<std::string> visited;
  size_t total = 0;
  std::function<absl::Status(const YAML::Node&, const std::vector<fs::path>&)> visit;
  visit = [&](const YAML::Node& node, const std::vector<fs::path>& search_roots) -> absl::Status {
    if (node.IsSequence()) {
      for (const auto& child : node) {
        const auto status = visit(child, search_roots);
        if (!status.ok())
          return status;
      }
    } else if (node.IsMap()) {
      for (const auto& entry : node) {
        if (!entry.first.IsScalar())
          continue;
        std::string key = entry.first.as<std::string>();
        std::replace(key.begin(), key.end(), '_', '-');
        const auto& value = entry.second;
        if (value.IsScalar() &&
            (key == "config-file" || key == "config-file-path" || key == "ll-config-file" ||
             key == "runtime-tuning-config-file")) {
          const fs::path requested(value.as<std::string>());
          if (requested.empty())
            continue;
          fs::path path = requested;
          if (path.is_relative()) {
            for (const auto& root : search_roots) {
              if (fs::exists(root / requested)) {
                path = root / requested;
                break;
              }
            }
          }
          path = fs::absolute(path).lexically_normal();
          if (!visited.insert(path.string()).second)
            continue;
          YAML::Node file;
          file["path"] = path.string();
          if (fs::is_directory(path)) {
            file["status"] = "directory-reference";
          } else if (!fs::is_regular_file(path)) {
            file["status"] = "not-found";
          } else {
            const auto bytes = fs::file_size(path);
            if (bytes > 16 * 1024 * 1024 || total + bytes > 64 * 1024 * 1024)
              return absl::ResourceExhaustedError(
                  "Telemetry text configurations exceed archive limits: " + path.string());
            std::ifstream input(path, std::ios::binary);
            if (!input)
              return absl::InternalError("Cannot archive configuration: " + path.string());
            const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            if (input.bad())
              return absl::InternalError("Cannot read configuration: " + path.string());
            total += contents.size();
            file["contents"] = contents;
            file["status"] = "captured";
            if (path.extension() == ".yaml" || path.extension() == ".yml") {
              auto nested_roots = search_roots;
              nested_roots.insert(nested_roots.begin(), path.parent_path());
              try {
                const auto status = visit(YAML::Load(contents), nested_roots);
                if (!status.ok())
                  return status;
              } catch (const YAML::Exception&) {
                // Raw contents remain authoritative, including non-YAML plugin files.
                file["nested-references"] = "unparsed";
              }
            }
          }
          files.push_back(file);
        } else {
          const auto status = visit(value, search_roots);
          if (!status.ok())
            return status;
        }
      }
    }
    return absl::OkStatus();
  };
  try {
    const auto status = visit(config, roots);
    if (!status.ok())
      return status;
    return files;
  } catch (const std::exception& error) {
    return absl::InternalError(std::string("Cannot archive referenced configuration: ") + error.what());
  }
}
