#include <cstddef>
#include <iostream>
#include <span>

#include "cli/app.hpp"

int main(int argc, char** argv) {
  const std::span<const char* const> args{argv, static_cast<std::size_t>(argc)};
  return dorq::cli::run(args, std::cout, std::cerr);
}
