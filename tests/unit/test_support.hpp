#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cli/app.hpp"

namespace dorq::test {

// A directory removed when the test ends.
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("dorq-test-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(path_);
    // A .git marker stops config discovery from walking above the directory.
    std::filesystem::create_directory(path_ / ".git");
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

inline void write_file(const std::filesystem::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

struct Result {
  int status;
  std::string out;
  std::string err;
};

// Runs the CLI in-process, in `cwd`, with `stdin_text` as standard input.
inline Result run_in(const std::filesystem::path& cwd, std::initializer_list<const char*> args,
                     std::string_view stdin_text = "", bool stdin_is_tty = false) {
  std::vector<const char*> argv{"dorq"};
  argv.insert(argv.end(), args);
  std::istringstream in{std::string{stdin_text}};
  std::ostringstream out;
  std::ostringstream err;
  dorq::cli::Io io{in, out, err, stdin_is_tty, false, cwd};
  const int status = dorq::cli::run(argv, io);
  return {status, out.str(), err.str()};
}

// The same, in a fresh temporary directory.
inline Result run(std::initializer_list<const char*> args, std::string_view stdin_text = "",
                  bool stdin_is_tty = false) {
  const TempDir dir;
  return run_in(dir.path(), args, stdin_text, stdin_is_tty);
}

inline bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// Lines of `text` that contain `needle`.
inline std::size_t count_matching(const std::string& text, const std::string& needle) {
  std::size_t count = 0;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    count += contains(line, needle) ? 1U : 0U;
  }
  return count;
}

}  // namespace dorq::test
