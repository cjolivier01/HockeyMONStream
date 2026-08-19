#pragma once

#include <functional>
#include <optional>
#include <string>
#include <map>
#include <vector>
namespace hm {

/**
 * @brief Launches a command in a specified working directory with a custom environment.
 *
 * This function forks a child process, changes its working directory (if provided),
 * sets the provided environment variables, and executes the command via execve().
 * The child's stdout and stderr are captured using pipes. As data is received from either
 * stream, it is buffered until a full line (terminated by a newline) is available.
 * Once a full line is captured, the callback is invoked with that line. For output from stdout,
 * the callback is called with the second parameter set, and for stderr, the first parameter is set.
 *
 * @param cmd A vector of strings representing the command line. The first element should be the executable.
 * @param working_dir The directory in which the command should be executed. If empty, no directory change is performed.
 * @param env An unordered_map representing the environment variables for the new process.
 * @param callback A function to be called when a full line is captured from stdout or stderr.
 *        For a stdout line, callback is called as callback("", line); for stderr, as callback(line, "").
 *
 * @return int The exit code of the child process, or -1 on error.
 */

int run_command(
    const std::vector<std::string>& cmd,
    const std::string& working_dir,
    const std::map<std::string, std::string>& env,
    std::function<void(const std::string&, const std::string&)> callback,
    const std::function<bool()>& is_cancelled = {});

std::optional<std::string> findExecutable(const std::string& executable, const std::vector<std::string>& envVars);
std::vector<std::string> splitPaths(const std::string& paths);

} // namespace hm
