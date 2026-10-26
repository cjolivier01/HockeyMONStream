#include "src/apps/hstream-ui/RuntimeOptions.h"

#include <array>

#include "hstream/src/libs/common/ConfigPath.h"
#include "hstream/src/libs/common/GpuMemoryProfile.h"

namespace {
bool lookup(const YAML::Node& root, const char* key, YAML::Node* value) {
  const auto found = hm::get_node(root, key);
  if (!found)
    return false;
  value->reset(*found);
  return true;
}
} // namespace

std::optional<unsigned> hm::ui_internal::configured_pipeline_gpu(const YAML::Node& effective_config) {
  YAML::Node configured;
#ifdef HSTREAM_USE_HIP
  if (!lookup(effective_config, "runtime.amd.gpu_id", &configured))
    return std::nullopt;
#else
  if (!lookup(effective_config, "pipeline.application.global-gpu-id", &configured) &&
      !lookup(effective_config, "pipeline.hmstitcher.gpu-id", &configured)) {
    return 0;
  }
#endif
  if (!configured.IsScalar())
    return std::nullopt;
  try {
    const int ordinal = configured.as<int>();
    return ordinal >= 0 ? std::optional<unsigned>(static_cast<unsigned>(ordinal)) : std::nullopt;
  } catch (const YAML::Exception&) {
    return std::nullopt;
  }
}

QString hm::ui_internal::configured_gpu_memory_profile(
    const YAML::Node& baseline_config,
    const YAML::Node& user_config) {
  struct ProfileAlias {
    int rank{-1};
    YAML::Node value;
  } dashed, underscored;
  const std::array<YAML::Node, 2> layers{baseline_config, user_config};
  for (int rank = 0; rank < static_cast<int>(layers.size()); ++rank) {
    YAML::Node runtime;
    if (lookup(layers[rank], "runtime", &runtime) && !runtime.IsMap()) {
      dashed = {};
      underscored = {};
      continue;
    }
    YAML::Node dashed_value;
    if (lookup(layers[rank], "runtime.gpu-memory-profile", &dashed_value)) {
      dashed = {rank, YAML::Clone(dashed_value)};
    }
    YAML::Node underscored_value;
    if (lookup(layers[rank], "runtime.gpu_memory_profile", &underscored_value)) {
      underscored = {rank, YAML::Clone(underscored_value)};
    }
  }
  auto scalar_profile = [](const ProfileAlias& candidate) -> std::optional<QString> {
    if (candidate.rank < 0 || !candidate.value.IsScalar())
      return std::nullopt;
    try {
      return QString::fromStdString(candidate.value.as<std::string>()).trimmed().toLower().replace('_', '-');
    } catch (const YAML::Exception&) {
      return std::nullopt;
    }
  };
  const ProfileAlias& preferred = dashed.rank >= underscored.rank ? dashed : underscored;
  const ProfileAlias& fallback = dashed.rank >= underscored.rank ? underscored : dashed;
  std::optional<QString> configured = scalar_profile(preferred);
  if (!configured)
    configured = scalar_profile(fallback);
  if (!configured)
    return "auto";
  QString profile = *configured;
  if (profile == "low-memory")
    profile = "low";
  if (profile == "full" || profile == "normal")
    profile = "standard";
  if (profile == "auto" || profile == "low" || profile == "standard") {
    return profile;
  }
  return {};
}

absl::StatusOr<QString> hm::ui_internal::resolve_gpu_memory_profile(
    const QString& requested,
    std::optional<unsigned> gpu,
    MemoryQuery query) {
  if (requested == "standard" || requested == "low")
    return requested;
  if (requested != "auto")
    return absl::InvalidArgumentError("GPU memory profile must be auto, standard, or low");
  if (!gpu)
    return absl::FailedPreconditionError("The configured GPU is unavailable");
  const auto bytes = query ? query(*gpu) : hm::inference::GpuTotalMemoryBytes(*gpu);
  if (!bytes.ok())
    return bytes.status();
  return hm::inference::UseLowMemoryProfile(*bytes) ? QStringLiteral("low") : QStringLiteral("standard");
}
