#include "hstream/src/apps/apps-common/deepstream_config_yaml.h"
#include <unistd.h>
#include <string>
#include <system_error>
#include "hstream/src/libs/common/filesystem.h"

#define _PATH_MAX 2048

// bool getAbsoluteFilePathYaml(const std::string& cfgFilePath, const std::string& filePath, std::string& absPathStr) {
//   fs::path fileP(filePath);

//   // If filePath is already absolute, try to canonicalize it.
//   if (fileP.is_absolute()) {
//     std::error_code ec;
//     fs::path canonicalPath = fs::canonical(fileP, ec);
//     if (ec) {
//       // If the error is because the file does not exist, use the normalized path.
//       if (ec.value() != static_cast<int>(std::errc::no_such_file_or_directory))
//         return false;
//       absPathStr = fileP.lexically_normal().string();
//     } else {
//       absPathStr = canonicalPath.string();
//     }
//     return true;
//   }

//   // For a relative filePath, first get the absolute path of the config file.
//   fs::path cfgP(cfgFilePath);
//   std::error_code ec;
//   fs::path canonicalCfg = fs::canonical(cfgP, ec);
//   if (ec)
//     return false;

//   // Get the directory of the configuration file.
//   fs::path cfgDir;
//   if (fs::is_directory(canonicalCfg)) {
//     cfgDir = canonicalCfg;
//   } else {
//     cfgDir = canonicalCfg.parent_path();
//   }

//   // Construct the combined path.
//   fs::path combinedPath = cfgDir / fileP;
//   fs::path canonicalCombined = fs::canonical(combinedPath, ec);
//   if (ec) {
//     // If the error is because the file doesn't exist, use the unresolved (but normalized) path.
//     if (ec.value() == static_cast<int>(std::errc::no_such_file_or_directory))
//       absPathStr = combinedPath.lexically_normal().string();
//     else
//       return false;
//   } else {
//     absPathStr = canonicalCombined.string();
//   }
//   return true;
// }

bool getAbsoluteFilePathYaml(const std::string& cfgFilePath, const std::string& filePath, std::string& absPathStr) {
  fs::path fileP(filePath);

  // If filePath is already absolute, try to canonicalize it.
  if (fileP.is_absolute()) {
    error_code_t ec;
    fs::path canonicalPath = fs::canonical(fileP, ec);
    if (ec) {
      // If the error is because the file does not exist, use the normalized path.
#if defined(__cpp_lib_filesystem) // || defined(__cpp_lib_experimental_filesystem)
      if (ec.value() != static_cast<int>(std::errc::no_such_file_or_directory))
#else
      if (ec.value() != static_cast<int>(boost::system::errc::no_such_file_or_directory))
#endif
        return false;
      absPathStr = fileP.lexically_normal().string();
    } else {
      absPathStr = canonicalPath.string();
    }
    return true;
  }

  // For a relative filePath, first get the absolute path of the config file.
  fs::path cfgP(cfgFilePath);
  error_code_t ec;
  fs::path canonicalCfg = fs::canonical(cfgP, ec);
  if (ec)
    return false;

  // Get the directory of the configuration file.
  fs::path cfgDir = fs::is_directory(canonicalCfg) ? canonicalCfg : canonicalCfg.parent_path();

  // Construct the combined path.
  fs::path combinedPath = cfgDir / fileP;

#if defined(__cpp_lib_filesystem) || defined(__cpp_lib_experimental_filesystem)
  fs::path canonicalCombined = fs::canonical(combinedPath, ec);
#else
  fs::path canonicalCombined = fs::canonical(combinedPath, cfgDir, ec); // Boost needs base path
#endif

  if (ec) {
#if defined(__cpp_lib_filesystem) // || defined(__cpp_lib_experimental_filesystem)
    if (ec.value() == static_cast<int>(std::errc::no_such_file_or_directory))
#else
    if (ec.value() == static_cast<int>(boost::system::errc::no_such_file_or_directory))
#endif
      absPathStr = combinedPath.lexically_normal().string();
    else
      return false;
  } else {
    absPathStr = canonicalCombined.string();
  }

  return true;
}

/* Separate a config file entry with delimiters
 * into strings. */
std::vector<std::string> split_string(std::string input) {
  std::vector<int> positions;
  for (unsigned int i = 0; i < input.size(); i++) {
    if (input[i] == ';')
      positions.push_back(i);
  }
  std::vector<std::string> ret;
  int prev = 0;
  for (auto& j : positions) {
    std::string temp = input.substr(prev, j - prev);
    ret.push_back(temp);
    prev = j + 1;
  }
  ret.push_back(input.substr(prev, input.size() - prev));
  return ret;
}

/* Get the absolute path of a file mentioned in the config given a
 * file path absolute/relative to the config file. */
gboolean get_absolute_file_path_yaml(const gchar* cfg_file_path, const gchar* file_path, char* abs_path_str) {
  gchar abs_cfg_path[PATH_MAX + 1];
  gchar abs_real_file_path[PATH_MAX + 1];
  gchar* abs_file_path;
  gchar* delim;

  /* Absolute path. No need to resolve further. */
  if (file_path[0] == '/') {
    /* Check if the file exists, return error if not. */
    if (!realpath(file_path, abs_real_file_path)) {
      /* Ignore error if file does not exist and use the unresolved path. */
      if (errno != ENOENT)
        return FALSE;
    }
    g_strlcpy(abs_path_str, abs_real_file_path, _PATH_MAX);
    return TRUE;
  }

  /* Get the absolute path of the config file. */
  if (!realpath(cfg_file_path, abs_cfg_path)) {
    return FALSE;
  }

  /* Remove the file name from the absolute path to get the directory of the
   * config file. */
  delim = g_strrstr(abs_cfg_path, "/");
  *(delim + 1) = '\0';

  /* Get the absolute file path from the config file's directory path and
   * relative file path. */
  abs_file_path = g_strconcat(abs_cfg_path, file_path, nullptr);

  /* Resolve the path.*/
  if (realpath(abs_file_path, abs_real_file_path) == nullptr) {
    /* Ignore error if file does not exist and use the unresolved path. */
    if (errno == ENOENT)
      g_strlcpy(abs_real_file_path, abs_file_path, _PATH_MAX);
    else {
      if (abs_file_path)
        g_free(abs_file_path);
      return FALSE;
    }
  }

  g_free(abs_file_path);

  g_strlcpy(abs_path_str, abs_real_file_path, _PATH_MAX);
  return TRUE;
}
