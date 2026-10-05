#pragma once

#include <stdexcept>
#include <string>

namespace dorq {

// The input cannot be read as a whole: a missing file, a missing date column,
// an unterminated quote. Maps to exit status 3. A bad value in one row is not an
// InputError; it is a FieldIssue, reported by DQ104.
class InputError : public std::runtime_error {
 public:
  explicit InputError(const std::string& message) : std::runtime_error(message) {}
};

}  // namespace dorq
