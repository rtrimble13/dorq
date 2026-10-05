#pragma once

#include <ostream>
#include <span>

namespace dorq::cli {

// Runs dorq with the given command line (args[0] is the program name) and returns
// the process exit status (see dorq/exit_code.hpp). Everything the command prints
// goes to `out` or `err` -- never straight to std::cout -- so the whole command-line
// contract can be tested in-process.
[[nodiscard]] int run(std::span<const char* const> args, std::ostream& out, std::ostream& err);

}  // namespace dorq::cli
