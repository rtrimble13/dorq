// M9, DR-0908: the share terms model, DQ921 share-terms-step and DQ922
// share-count-mismatch, through the CLI.

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "dorq/date.hpp"
#include "test_support.hpp"

using dorq::Date;
using dorq::test::contains;
using dorq::test::Result;
using dorq::test::run_in;
using dorq::test::TempDir;
using dorq::test::write_file;

namespace {

std::vector<std::string> lines_with(const std::string& text, const std::string& needle) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    if (contains(line, needle)) {
      out.push_back(line);
    }
  }
  return out;
}

std::string field_of(const std::string& line, const std::string& name) {
  const std::string open = "\"" + name + "\":\"";
  const auto from = line.find(open);
  if (from == std::string::npos) {
    return {};
  }
  const auto start = from + open.size();
  return line.substr(start, line.find('"', start) - start);
}

// The reports of `code` on `series` at warn or above.
std::vector<std::string> reports(const std::string& out, const std::string& code,
                                 const std::string& series) {
  std::vector<std::string> got;
  for (const std::string& line : lines_with(out, R"("code":")" + code + "\"")) {
    if (field_of(line, "series") == series && !contains(line, R"("severity":"info")")) {
      got.push_back(line);
    }
  }
  return got;
}

constexpr const char* kHeader =
    "security_id,statement,period_type,fiscal_date,fiscal_year,fiscal_period,version_no,"
    "known_from,known_to,knowledge_source,filing_date,close,revenue,weighted_average_shares,"
    "weighted_average_shares_diluted,total_assets,shares_outstanding\n";

constexpr const char* kItems =
    "column,statement,item_class,sign_convention,is_subtotal\n"
    "revenue,income,flow,positive_is_inflow,false\n"
    "weighted_average_shares,income,share_count,signed,false\n"
    "weighted_average_shares_diluted,income,share_count,signed,false\n"
    "total_assets,balance_sheet,stock,signed,true\n"
    "shares_outstanding,balance_sheet,share_count,signed,false\n";

Date quarter_end(int q) {
  const int year = 2010 + q / 4;
  const std::array<std::pair<int, int>, 4> ends{{{3, 31}, {6, 30}, {9, 30}, {12, 31}}};
  const auto [month, day] = ends.at(static_cast<std::size_t>(q % 4));
  return Date::from_ymd(year, month, day);
}

Date plus(Date d, int days) { return Date::from_days(d.days() + days); }

// A split the company made: its ex-date and its ratio, shares after over before.
struct Split {
  Date ex;
  double ratio = 1.0;
};

// What a quarter's counts are, in the company's first terms, before any split.
struct Counts {
  double weighted = 0.0;
  double diluted = 0.0;
  double outstanding = 0.0;
  double close = 0.0;  // the price at the filing, in first terms
};

// A fault's or an event's change to a quarter's stored counts (in the terms they are
// stored in), before they are written.
using Edit = std::function<void(int, Counts&)>;

struct Company {
  std::string id;
  std::vector<Split> splits;  // every split the company made, on file or not
  // The counts in first terms, by quarter: by default a slow buyback at a steady
  // price. Changed for an offering or a merger.
  std::function<double(int)> outstanding = [](int q) { return 1e8 * std::pow(0.996, q); };
  std::function<double(int)> price = [](int q) { return 60.0 * (1.0 + 0.01 * q); };
  std::function<double(int)> weighted;  // default: the mean of the quarter's two ends
  Edit edit;
  int quarters = 28;
  // A restated income row: (quarter, the date it became known).
  std::vector<std::pair<int, Date>> restated;
};

double terms_at(const Company& c, Date known) {
  double f = 1.0;
  for (const Split& s : c.splits) {
    if (s.ex <= known) {
      f *= s.ratio;
    }
  }
  return f;
}

std::string number(double v) { return std::to_string(std::llround(v)); }

std::string price_text(double v) {
  std::ostringstream out;
  out.precision(2);
  out << std::fixed << v;
  return out.str();
}

// One income and one balance-sheet row of a quarter, version `version`, known from
// `known` (and to `known_to`), its counts in the terms of `known`.
std::string rows(const Company& c, int q, int version, Date known, const std::string& known_to) {
  const Date end = quarter_end(q);
  const double f = terms_at(c, known);
  Counts counts;
  counts.outstanding = c.outstanding(q) * f;
  counts.weighted =
      (c.weighted ? c.weighted(q) : 0.5 * (c.outstanding(q) + c.outstanding(q == 0 ? 0 : q - 1))) *
      f;
  counts.diluted = counts.weighted * 1.01;
  counts.close = c.price(q) / f;
  if (c.edit && version == 1) {
    c.edit(q, counts);
  }
  const std::string key = c.id + ",";
  const std::string tail = "," + end.to_string() + "," + std::to_string(2010 + q / 4) + ",Q" +
                           std::to_string(q % 4 + 1) + "," + std::to_string(version) + "," +
                           known.to_string() + "," + known_to + ",filed," + known.to_string() +
                           "," + price_text(counts.close) + ",";
  std::string text = key + "income,quarter" + tail;
  text += number(1e9 * (1.0 + 0.02 * q)) + "," + number(counts.weighted) + "," +
          number(counts.diluted) + ",,\n";
  if (version == 1) {
    text += key + "balance_sheet,quarter" + tail;
    text += ",,," + number(1e10 * (1.0 + 0.01 * q)) + "," + number(counts.outstanding) + "\n";
  }
  return text;
}

// The company's quarters, each filed 40 days after its end, and its restated rows.
std::string company(const Company& c) {
  std::string text;
  for (int q = 0; q < c.quarters; ++q) {
    std::string known_to;
    for (const auto& [restated, when] : c.restated) {
      if (restated == q) {
        known_to = when.to_string();
      }
    }
    text += rows(c, q, 1, plus(quarter_end(q), 40), known_to);
    for (const auto& [restated, when] : c.restated) {
      if (restated == q) {
        text += rows(c, q, 2, when, "");
      }
    }
  }
  return text;
}

// The actions file: these splits on file, as fafnir writes them.
std::string actions(const std::vector<std::pair<std::string, Split>>& on_file) {
  std::string text =
      "security_id,ex_date,action_type,split_numerator,split_denominator,dividend_amount\n";
  for (const auto& [id, split] : on_file) {
    const bool forward = split.ratio >= 1.0;
    const double big = forward ? split.ratio : 1.0 / split.ratio;
    text += id + "," + split.ex.to_string() + ",split," + (forward ? number(big) : "1") + "," +
            (forward ? "1" : number(big)) + ",\n";
  }
  return text;
}

Result check(const TempDir& dir, const std::string& csv, const std::string* actions_csv) {
  write_file(dir.path() / "s.csv", std::string{kHeader} + csv);
  write_file(dir.path() / "items.csv", kItems);
  const std::string file = (dir.path() / "s.csv").string();
  const std::string items = (dir.path() / "items.csv").string();
  if (actions_csv == nullptr) {
    return run_in(dir.path(), {"--items", items.c_str(), "--select", "DQ92", "--show-info",
                               "--format", "jsonl", "--exit-zero", file.c_str()});
  }
  write_file(dir.path() / "actions.csv", *actions_csv);
  const std::string actions_file = (dir.path() / "actions.csv").string();
  return run_in(dir.path(),
                {"--items", items.c_str(), "--actions", actions_file.c_str(), "--select", "DQ92",
                 "--show-info", "--format", "jsonl", "--exit-zero", file.c_str()});
}

}  // namespace

TEST_CASE("DQ921: a split missing from the actions is one context gap, its window narrowed") {
  const TempDir dir;
  // A 2:1 split on 2013-05-20, between the 2013 Q1 filing (2013-05-10) and the Q2
  // one (2013-08-09), that the actions lack; the 2011 Q4 income statement, restated
  // in 2015, is in the terms after it, and so steps against the quarters either side.
  Company c;
  c.id = "MISSING";
  c.splits = {{Date::from_ymd(2013, 5, 20), 2.0}};
  c.restated = {{7, Date::from_ymd(2015, 3, 2)}};
  const std::string none = actions({});
  const Result r = check(dir, company(c), &none);
  INFO(r.err);
  REQUIRE(r.status == 0);
  const std::vector<std::string> got = reports(r.out, "DQ921", "MISSING");
  REQUIRE(got.size() == 1);
  const std::string& f = got.front();
  CHECK(contains(f, R"("fiscal_date":"2013-06-30")"));
  CHECK(contains(f, R"("classification":"context_gap")"));
  CHECK(contains(f,
                 "with no split on file: a 2:1 split missing from the actions, its ex-date "
                 "after 2013-05-10 and on or before 2013-08-09"));
  CHECK(contains(f, "it explains"));
  CHECK(contains(f, R"("suggested_action":{"kind":"add_split","ratio":"2:1",)"
                    R"("ex_date_after":"2013-05-10","ex_date_on_or_before":"2013-08-09")"));
  CHECK(contains(f, R"("unreported_split")"));
  // The mismatch the restated row makes with the balance sheet is the same split's.
  CHECK(reports(r.out, "DQ922", "MISSING").empty());

  // Without --actions there is nothing to put the counts into one terms with:
  // DQ921 judges nothing.
  const Result without = check(dir, company(c), nullptr);
  CHECK(reports(without.out, "DQ921", "MISSING").empty());
}

TEST_CASE("DQ921: a count stored in the terms before a split on file is a data error") {
  const TempDir dir;
  // A 2:1 split on file on 2012-08-01; the 2012 Q2 10-Q, filed 2012-08-09, keeps
  // its counts in the terms before it.
  Company c;
  c.id = "TERMS";
  c.splits = {{Date::from_ymd(2012, 8, 1), 2.0}};
  c.edit = [](int q, Counts& counts) {
    if (q == 9) {
      counts.weighted /= 2.0;
      counts.diluted /= 2.0;
      counts.outstanding /= 2.0;
    }
  };
  const std::string on_file = actions({{"TERMS", c.splits.front()}});
  const Result r = check(dir, company(c), &on_file);
  INFO(r.err);
  REQUIRE(r.status == 0);
  const std::vector<std::string> got = reports(r.out, "DQ921", "TERMS");
  REQUIRE(got.size() == 3);  // each count, once: the step back is the same error's end
  for (const std::string& f : got) {
    CHECK(contains(f, R"("fiscal_date":"2012-06-30")"));
    CHECK(contains(f, R"("classification":"data_error")"));
    CHECK(contains(f,
                   "against the split on file (2:1 2012-08-01): a count stored in the wrong "
                   "share terms"));
    CHECK(contains(f, R"("kind":"check_share_terms")"));
  }
  CHECK(reports(r.out, "DQ922", "TERMS").empty());
}

TEST_CASE("DQ921: no warn on a split on file, an offering or a merger") {
  const TempDir dir;
  std::string csv;
  // A 3:1 split on file, and a 1:10 reverse split at a low price.
  Company split;
  split.id = "SPLIT";
  split.splits = {{Date::from_ymd(2012, 2, 10), 3.0}, {Date::from_ymd(2015, 2, 2), 0.1}};
  split.price = [](int q) { return q < 20 ? 90.0 : 6.0; };
  csv += company(split);
  // An offering in 2012 Q3: half as many shares again, sold at the market price, so
  // the price holds and the market cap rises; the average moves by part of it.
  Company offer;
  offer.id = "OFFER";
  offer.outstanding = [](int q) { return 1e8 * (q >= 10 ? 1.5 : 1.0); };
  offer.weighted = [](int q) {
    if (q == 10) {
      return 1.2e8;  // the quarter of the offering: part of it
    }
    return 1e8 * (q > 10 ? 1.5 : 1.0);
  };
  offer.price = [](int) { return 40.0; };
  csv += company(offer);
  // A merger paid in shares at the start of 2013 Q3: the count doubles for the
  // whole quarter, and the market cap with it.
  Company merger;
  merger.id = "MERGER";
  merger.outstanding = [](int q) { return 1e8 * (q >= 14 ? 2.0 : 1.0); };
  merger.weighted = merger.outstanding;
  merger.price = [](int) { return 80.0; };
  csv += company(merger);
  const std::string on_file = actions({{"SPLIT", split.splits[0]}, {"SPLIT", split.splits[1]}});
  const Result r = check(dir, csv, &on_file);
  INFO(r.err);
  REQUIRE(r.status == 0);
  for (const std::string id : {"SPLIT", "OFFER", "MERGER"}) {
    INFO(id);
    CHECK(reports(r.out, "DQ921", id).empty());
    CHECK(reports(r.out, "DQ922", id).empty());
  }
}

TEST_CASE("DQ922: a weighted-average count at a power of ten against the outstanding count") {
  const TempDir dir;
  std::string csv;
  // The 2013 Q1 weighted-average count stored at x0.001.
  Company c;
  c.id = "MISMATCH";
  c.edit = [](int q, Counts& counts) {
    if (q == 12) {
      counts.weighted = std::round(counts.weighted * 0.001);
    }
  };
  csv += company(c);
  // A 2:1 split on file on 2012-07-20, after the 2012 Q2 period and before its
  // filing: the cover's outstanding count is in the new terms, the period's average
  // in the old. The split explains the two apart (DQ921 has the average's terms).
  Company cover;
  cover.id = "COVER";
  cover.splits = {{Date::from_ymd(2012, 7, 20), 2.0}};
  cover.edit = [](int q, Counts& counts) {
    if (q == 9) {
      counts.weighted /= 2.0;
      counts.diluted /= 2.0;
    }
  };
  csv += company(cover);
  const std::string on_file = actions({{"COVER", cover.splits.front()}});
  for (const bool with_actions : {true, false}) {
    INFO(with_actions);
    const Result r = check(dir, csv, with_actions ? &on_file : nullptr);
    INFO(r.err);
    REQUIRE(r.status == 0);
    const std::vector<std::string> got = reports(r.out, "DQ922", "MISMATCH");
    REQUIRE(got.size() == 1);
    const std::string& f = got.front();
    CHECK(contains(f, R"("fiscal_date":"2013-03-31")"));
    CHECK(contains(f, R"("item":"weighted_average_shares")"));
    CHECK(contains(f, "and shares_outstanding ("));
    CHECK(contains(f, "disagree by x0.001, which no split on file explains"));
    CHECK(contains(f, R"("classification":"data_error")"));
    CHECK(contains(f, R"("kind":"check_share_terms")"));
    if (with_actions) {
      CHECK(reports(r.out, "DQ922", "COVER").empty());
    }
  }
}
