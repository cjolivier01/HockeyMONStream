#include "PipelineRuntimeEnvironment.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

bool environment_equals(const char* expected) {
  const char* value = std::getenv("USE_NEW_NVSTREAMMUX");
  return value && std::strcmp(value, expected) == 0;
}

} // namespace

int main() {
  bool ok = true;

  ::unsetenv("USE_NEW_NVSTREAMMUX");
  ok &=
      expect(hm::pipeline_internal::configure_streammux_runtime_environment(), "Default streammux setup must succeed");
  ok &= expect(environment_equals("yes"), "An unset streammux selection must default to the replacement mux");

  ::setenv("USE_NEW_NVSTREAMMUX", "no", /*overwrite=*/1);
  ok &=
      expect(hm::pipeline_internal::configure_streammux_runtime_environment(), "Explicit streammux setup must succeed");
  ok &= expect(environment_equals("no"), "An explicit legacy-mux override must be preserved");

  ::setenv("USE_NEW_NVSTREAMMUX", "", /*overwrite=*/1);
  ok &= expect(hm::pipeline_internal::configure_streammux_runtime_environment(), "Empty streammux setup must succeed");
  ok &= expect(environment_equals("yes"), "An empty streammux selection must use the replacement mux");

  return ok ? 0 : 1;
}
