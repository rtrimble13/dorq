#pragma once

// dorq-synth's statements universe (tools/synth/README.md, plan item DR-0903):
// companies whose identities hold by construction, with labelled faults and hard
// negatives, for the DQ9xx checks' gate.

#include <cstdint>
#include <filesystem>

namespace synth {

// Writes the universe for `seed` to `dir`: statements.csv, items.csv,
// actions.csv, meta.csv, labels.csv and labels.jsonl. The same seed gives the
// same bytes.
void generate_statements(std::uint64_t seed, const std::filesystem::path& dir);

// Reads dir/statements.csv through dorq's reader, with dir/items.csv, and holds
// the identities, bounds and non-negative items of `config`, the quarters'
// sum against the year and the change in cash against the cash flow, on every
// row no fault label covers. Prints a line per rule. Returns the number of
// breaks, or -1 when a rule judges no row or the input has a row dorq cannot
// use (DQ104, DQ109).
int verify_statements(const std::filesystem::path& dir, const std::filesystem::path& config);

}  // namespace synth
