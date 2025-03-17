#include "hstream/src/apps/apps-common/deepstream_config_yaml.h"
#include <filesystem>
#include <string>
#include <system_error>

bool get_absolute_file_path_yaml(const std::string& cfgFilePath, const std::string& filePath, std::string& absPathStr) {
  namespace fs = std::filesystem;
  fs::path fileP(filePath);

  // If filePath is already absolute, try to canonicalize it.
  if (fileP.is_absolute()) {
    std::error_code ec;
    fs::path canonicalPath = fs::canonical(fileP, ec);
    if (ec) {
      // If the error is because the file does not exist, use the normalized path.
      if (ec.value() != static_cast<int>(std::errc::no_such_file_or_directory))
        return false;
      absPathStr = fileP.lexically_normal().string();
    } else {
      absPathStr = canonicalPath.string();
    }
    return true;
  }

  // For a relative filePath, first get the absolute path of the config file.
  fs::path cfgP(cfgFilePath);
  std::error_code ec;
  fs::path canonicalCfg = fs::canonical(cfgP, ec);
  if (ec)
    return false;

  // Get the directory of the configuration file.
  fs::path cfgDir = canonicalCfg.parent_path();

  // Construct the combined path.
  fs::path combinedPath = cfgDir / fileP;
  fs::path canonicalCombined = fs::canonical(combinedPath, ec);
  if (ec) {
    // If the error is because the file doesn't exist, use the unresolved (but normalized) path.
    if (ec.value() == static_cast<int>(std::errc::no_such_file_or_directory))
      absPathStr = combinedPath.lexically_normal().string();
    else
      return false;
  } else {
    absPathStr = canonicalCombined.string();
  }
  return true;
}
