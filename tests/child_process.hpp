#pragma once

// A subprocess running a real compiled binary, for tests that exercise
// the actual deployment shape (real processes on real sockets). RAII:
// SIGTERM on destruction. Adapted from sequencer's
// examples/counter/tests/child_process.hpp, plus wait() for a tool
// that is expected to exit on its own.

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace exchange::test {

class ChildProcess {
 public:
  ChildProcess(std::string path, std::vector<std::string> args) : path_(std::move(path)), args_(std::move(args)) {
    std::vector<char*> argv;
    argv.push_back(path_.data());
    for (auto& arg : args_) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    pid_ = ::fork();
    if (pid_ < 0) {
      throw std::runtime_error("ChildProcess: fork failed");
    }
    if (pid_ == 0) {
      ::execv(path_.c_str(), argv.data());
      ::_exit(127);
    }
  }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess() {
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      int status = 0;
      ::waitpid(pid_, &status, 0);
    }
  }

  pid_t pid() const { return pid_; }
  void kill(int signal) { ::kill(pid_, signal); }

  // Waits for the process to exit; returns its exit code (or -1 if it
  // was killed by a signal).
  int wait() {
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }

 private:
  std::string path_;
  std::vector<std::string> args_;
  pid_t pid_ = -1;
};

}  // namespace exchange::test
