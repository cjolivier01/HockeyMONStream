#include <NvInferRuntime.h>
#include <dlfcn.h>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2)
    return 1;
  const void* original = nullptr;
  for (int i = 0; i < 3; ++i) {
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      std::cerr << dlerror() << '\n';
      return 1;
    }
    const auto* creator = getPluginRegistry()->getPluginCreator("YoloLayer_TRT", "1");
    if (!creator || (original && original != creator))
      return 2;
    original = creator;
    dlclose(library);
    // DeepStream closes the library when retiring an inference context. The
    // process registry still owns this pointer and its virtual methods.
    if (std::string(creator->getPluginName()) != "YoloLayer_TRT")
      return 3;
  }
  return 0;
}
