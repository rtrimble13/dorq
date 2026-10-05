#pragma once

#include <filesystem>
#include <istream>
#include <ostream>
#include <span>

namespace dorq::cli {

// Everything a run touches outside its arguments, so the whole command-line
// contract can be tested in-process.
struct Io {
  std::istream& in;
  std::ostream& out;
  std::ostream& err;
  bool stdin_is_tty = false;
  // Whether text output should be coloured when --color is auto: stdout is a
  // terminal and NO_COLOR is unset.
  bool color = false;
  // Where config discovery starts.
  std::filesystem::path cwd;
};

// Runs dorq with the given command line (args[0] is the program name) and returns
// the process exit status (see dorq/exit_code.hpp).
[[nodiscard]] int run(std::span<const char* const> args, Io& io);

}  // namespace dorq::cli
