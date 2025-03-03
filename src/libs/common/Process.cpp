#include "src/libs/common/Process.h"

#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace hm {

/// run_command launches a command given by cmd (a vector of strings)
/// in the directory working_dir, with the environment variables in env.
/// As the command runs, it reads from its stdout and stderr (the callback’s
/// parameters are interpreted as (stderr_line, stdout_line))—whenever a full
/// line is available from one of these streams (i.e. terminated by a newline),
/// the callback is invoked with that line (and the other string empty).
/// After the process terminates, the function returns the process’s exit code.
int run_command(
    const std::vector<std::string>& cmd,
    const std::string& working_dir,
    const std::unordered_map<std::string, std::string>& env,
    std::function<void(const std::string&, const std::string&)> callback) {
  if (cmd.empty()) {
    std::cerr << "Error: command is empty." << std::endl;
    return -1;
  }

  // Create two pipes: one for stdout and one for stderr.
  int pipe_stdout[2];
  int pipe_stderr[2];
  if (pipe(pipe_stdout) == -1) {
    perror("pipe (stdout)");
    return -1;
  }
  if (pipe(pipe_stderr) == -1) {
    perror("pipe (stderr)");
    close(pipe_stdout[0]);
    close(pipe_stdout[1]);
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    close(pipe_stdout[0]);
    close(pipe_stdout[1]);
    close(pipe_stderr[0]);
    close(pipe_stderr[1]);
    return -1;
  }

  if (pid == 0) {
    // In child process

    // Change working directory if provided.
    if (!working_dir.empty() && chdir(working_dir.c_str()) != 0) {
      perror("chdir");
      _exit(1);
    }

    // Build environment array.
    std::vector<std::string> env_strings;
    std::vector<char*> envp;
    for (const auto& kv : env) {
      env_strings.push_back(kv.first + "=" + kv.second);
    }
    for (auto& s : env_strings) {
      envp.push_back(const_cast<char*>(s.c_str()));
    }
    envp.push_back(nullptr);

    // Redirect stdout and stderr.
    // Close the read ends; the child only writes.
    close(pipe_stdout[0]);
    close(pipe_stderr[0]);
    if (dup2(pipe_stdout[1], STDOUT_FILENO) == -1) {
      perror("dup2 stdout");
      _exit(1);
    }
    if (dup2(pipe_stderr[1], STDERR_FILENO) == -1) {
      perror("dup2 stderr");
      _exit(1);
    }
    // Close the original write ends.
    close(pipe_stdout[1]);
    close(pipe_stderr[1]);

    // Build the argument list for execve.
    std::vector<char*> argv;
    for (const auto& arg : cmd) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    // Execute the command with the given environment.
    if (execve(argv[0], argv.data(), envp.data()) == -1) {
      perror("execve");
      _exit(1);
    }
  }
  // In parent process

  // Close write ends as we only need to read.
  close(pipe_stdout[1]);
  close(pipe_stderr[1]);

  // Set pipes to non-blocking mode.
  fcntl(pipe_stdout[0], F_SETFL, O_NONBLOCK);
  fcntl(pipe_stderr[0], F_SETFL, O_NONBLOCK);

  std::string buffer_stdout;
  std::string buffer_stderr;
  const int BUFSIZE = 1024;
  char temp_buffer[BUFSIZE];

  bool stdout_open = true;
  bool stderr_open = true;

  // Loop while either pipe is open.
  while (stdout_open || stderr_open) {
    fd_set readfds;
    FD_ZERO(&readfds);
    int maxfd = -1;
    if (stdout_open) {
      FD_SET(pipe_stdout[0], &readfds);
      if (pipe_stdout[0] > maxfd)
        maxfd = pipe_stdout[0];
    }
    if (stderr_open) {
      FD_SET(pipe_stderr[0], &readfds);
      if (pipe_stderr[0] > maxfd)
        maxfd = pipe_stderr[0];
    }
    int ret = select(maxfd + 1, &readfds, nullptr, nullptr, nullptr);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      perror("select");
      break;
    }

    // Check for data from stdout.
    if (stdout_open && FD_ISSET(pipe_stdout[0], &readfds)) {
      ssize_t count = read(pipe_stdout[0], temp_buffer, BUFSIZE);
      if (count > 0) {
        buffer_stdout.append(temp_buffer, count);
        // Process complete lines.
        size_t pos;
        while ((pos = buffer_stdout.find('\n')) != std::string::npos) {
          std::string line = buffer_stdout.substr(0, pos);
          buffer_stdout.erase(0, pos + 1);
          // For a line from stdout, call callback with second parameter filled.
          callback("", line);
        }
      } else if (count == 0) {
        // EOF on stdout.
        stdout_open = false;
        close(pipe_stdout[0]);
        if (!buffer_stdout.empty()) {
          callback("", buffer_stdout);
          buffer_stdout.clear();
        }
      } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("read stdout");
        stdout_open = false;
        close(pipe_stdout[0]);
      }
    }

    // Check for data from stderr.
    if (stderr_open && FD_ISSET(pipe_stderr[0], &readfds)) {
      ssize_t count = read(pipe_stderr[0], temp_buffer, BUFSIZE);
      if (count > 0) {
        buffer_stderr.append(temp_buffer, count);
        // Process complete lines.
        size_t pos;
        while ((pos = buffer_stderr.find('\n')) != std::string::npos) {
          std::string line = buffer_stderr.substr(0, pos);
          buffer_stderr.erase(0, pos + 1);
          // For a line from stderr, call callback with first parameter filled.
          callback(line, "");
        }
      } else if (count == 0) {
        // EOF on stderr.
        stderr_open = false;
        close(pipe_stderr[0]);
        if (!buffer_stderr.empty()) {
          callback(buffer_stderr, "");
          buffer_stderr.clear();
        }
      } else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("read stderr");
        stderr_open = false;
        close(pipe_stderr[0]);
      }
    }
  }

  // Wait for the child process to finish.
  int status = 0;
  if (waitpid(pid, &status, 0) == -1) {
    perror("waitpid");
    return -1;
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return -1;
}

} // namespace hm
