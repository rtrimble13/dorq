#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <ostream>
#include <string>

#include "config/config.hpp"
#include "engine/engine.hpp"

namespace dorq {

// Totals over everything reported.
struct Summary {
  std::size_t inputs = 0;
  std::size_t series = 0;
  std::size_t rows = 0;
  std::size_t violations = 0;
  std::array<std::size_t, 3> by_severity{};  // indexed by Severity
  std::map<std::string, std::size_t> by_code;
  std::map<std::string, std::string> names;  // code -> check name
};

struct WriterOptions {
  bool color = false;  // text only
  std::string version;
  std::string config_hash;
  std::string fafnir_table = "core.daily_price";
};

// Writes violations as they arrive, series by series. See doc/output.md for each
// format.
class Writer {
 public:
  Writer() = default;
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer(Writer&&) = delete;
  Writer& operator=(Writer&&) = delete;
  virtual ~Writer() = default;

  virtual void begin() {}
  virtual void series(const SeriesResult& result) = 0;
  virtual void end(const Summary& /*summary*/) {}
};

[[nodiscard]] std::unique_ptr<Writer> make_writer(OutputFormat format, std::ostream& out,
                                                  WriterOptions options);

// Counts per code, as flake8 --statistics prints them.
[[nodiscard]] std::string statistics_text(const Summary& summary);

// Feeds a writer and keeps the summary.
class Report final : public ResultSink {
 public:
  explicit Report(Writer& writer) : writer_(writer) {}
  void series_done(SeriesResult&& result) override;
  [[nodiscard]] Summary& summary() noexcept { return summary_; }

 private:
  Writer& writer_;
  Summary summary_;
};

}  // namespace dorq
