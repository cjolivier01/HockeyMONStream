#include "src/apps/hstream-ui/RuntimeOptions.h"

#include <iostream>

int main() {
  using namespace hm::ui_internal;
  constexpr uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
  int queries = 0;
  auto query = [&](unsigned gpu) -> absl::StatusOr<uint64_t> {
    ++queries;
    if (gpu == 2)
      return 8 * gib;
    if (gpu == 3)
      return 8 * gib + 1;
    return absl::UnavailableError("device is unavailable");
  };
  const auto small = resolve_gpu_memory_profile("auto", 2, query);
  const auto large = resolve_gpu_memory_profile("auto", 3, query);
  if (!small.ok() || *small != "low" || !large.ok() || *large != "standard")
    return 1;
  const auto unavailable = resolve_gpu_memory_profile("auto", 4, query);
  const auto missing = resolve_gpu_memory_profile("auto", std::nullopt, query);
  if (unavailable.ok() || missing.ok() || queries != 3)
    return 1;
  for (const QString explicit_profile : {QStringLiteral("standard"), QStringLiteral("low")}) {
    const auto resolved = resolve_gpu_memory_profile(explicit_profile, 4, query);
    if (!resolved.ok() || *resolved != explicit_profile || queries != 3)
      return 1;
  }
  if (resolve_gpu_memory_profile("unknown", 2, query).ok() || queries != 3)
    return 1;
  const auto baseline = YAML::Load("runtime: {gpu_memory_profile: auto}");
  const auto user = YAML::Load("runtime: {gpu-memory-profile: low}");
  if (configured_gpu_memory_profile(baseline, user) != "low" ||
      configured_gpu_memory_profile(baseline, YAML::Load("{}")) != "auto" ||
      configured_gpu_memory_profile(
          baseline, YAML::Load("runtime: {gpu-memory-profile: normal, gpu_memory_profile: low}")) != "standard")
    return 1;
  std::cout << "PASS: shared Qt/runtime profile resolution, aliases and unavailable devices\n";
  return 0;
}
