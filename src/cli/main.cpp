#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <system_error>

#include <unistd.h>

#include "cli/app.hpp"

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  const char* no_color = std::getenv("NO_COLOR");  // NOLINT(concurrency-mt-unsafe): one thread here
  dorq::cli::Io io{
      .in = std::cin,
      .out = std::cout,
      .err = std::cerr,
      .stdin_is_tty = isatty(fileno(stdin)) == 1,
      .color = isatty(fileno(stdout)) == 1 && (no_color == nullptr || *no_color == '\0'),
      .cwd = ec ? std::filesystem::path{"."} : cwd,
  };
  const std::span<const char* const> args{argv, static_cast<std::size_t>(argc)};
  const int status = dorq::cli::run(args, io);
  std::cout.flush();
  return status;
}
