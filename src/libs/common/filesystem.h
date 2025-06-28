#pragma once

#include <iostream>
#include <string>

// Prefer std::filesystem if available
#if __has_include(<filesystem>) && (__cplusplus >= 201703L)
  #include <filesystem>
  namespace fs = std::filesystem;
  using error_code_t = std::error_code;
// #elif __has_include(<experimental/filesystem>)
//   #include <experimental/filesystem>
//   namespace fs = std::experimental::filesystem;
//   using error_code_t = std::error_code;
#else
  #include <boost/filesystem.hpp>
  #include <boost/system/error_code.hpp>
  namespace fs = boost::filesystem;
  using error_code_t = boost::system::error_code;
#endif

inline std::string normalize_path(const fs::path& p) {
#if defined(__cpp_lib_filesystem)
    // std::filesystem or experimental
    return p.lexically_normal().string();
#else
    // Boost fallback (Boost has lexically_normal too)
    return p.lexically_normal().string();
#endif
}

bool getAbsoluteFilePath(const std::string& cfgFilePath, const std::string& relativeFilePath, std::string& absPathStr) {
  fs::path fileP(relativeFilePath);

  if (fileP.is_absolute()) {
    absPathStr = normalize_path(fileP);
    return true;
  }

  fs::path base(cfgFilePath);
  fs::path dir = base.parent_path();
  fs::path combinedPath = dir / fileP;

#if defined(__cpp_lib_filesystem) // || defined(__cpp_lib_experimental_filesystem)
  std::error_code ec;
  fs::path canonicalCombined = fs::canonical(combinedPath, ec);
  absPathStr = canonicalCombined.empty() ? normalize_path(combinedPath) : canonicalCombined.string();
#else
  boost::system::error_code ec;
  fs::path canonicalCombined = fs::canonical(combinedPath, dir, ec);
  absPathStr = canonicalCombined.empty() ? normalize_path(combinedPath) : canonicalCombined.string();
#endif

  return true;
}
