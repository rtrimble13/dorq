// dorq-synth statements: a universe of companies' financial statements whose
// identities hold by construction, with labelled faults and hard negatives
// (tools/synth/README.md, plan item DR-0903).

#include "statements.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "context/context.hpp"
#include "dorq/calendar.hpp"
#include "dorq/date.hpp"
#include "dorq/number.hpp"
#include "dorq/series.hpp"
#include "dorq/statement.hpp"
#include "io/columns.hpp"
#include "io/csv.hpp"
#include "io/reader.hpp"
#include "rng.hpp"

namespace synth {
namespace {

using dorq::Date;
using dorq::ItemClass;
using dorq::KnowledgeSource;
using dorq::PeriodType;
using dorq::SignConvention;
using dorq::Statement;

constexpr double kNull = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t kNoQuarter = 0;  // no quarter

Date plus_days(Date date, int days) { return Date::from_days(date.days() + days); }

// fafnir's backfill loaded every row known by then on this day, and dated the
// restatements it found that SF1 gave no filing date for to it. Nightly loads
// follow.
Date backfill_day() { return Date::from_ymd(2026, 10, 20); }
// The newest period end, and the end of the universe's trading.
Date newest_end() { return Date::from_ymd(2026, 6, 30); }
Date last_session() { return Date::from_ymd(2026, 10, 30); }
// The cash flow statement's cash includes restricted cash from here (ASU 2016-18).
Date restricted_cash_rule() { return Date::from_ymd(2018, 1, 1); }

// ---------------------------------------------------------------------------
// The line items: fafnir's ref.statement_line_item (its migration 0035), in its
// order, under its column names.

enum Item : std::uint8_t {
  // income
  kRevenue,
  kCostOfRevenue,
  kGrossProfit,
  kResearch,
  kSga,
  kOperatingExpenses,
  kOperatingIncome,
  kInterestExpense,
  kEbit,
  kIncomeBeforeTax,
  kIncomeTax,
  kConsolidatedIncome,
  kNciIncome,
  kDiscontinued,
  kNetIncome,
  kPreferredDividends,
  kNetIncomeCommon,
  kSharesWeighted,
  kSharesDiluted,
  // balance sheet
  kCash,
  kInvestmentsCurrent,
  kInvestmentsNoncurrent,
  kInvestments,
  kReceivables,
  kInventory,
  kTaxAssets,
  kCurrentAssets,
  kPpe,
  kIntangibles,
  kNoncurrentAssets,
  kAssets,
  kPayables,
  kDeferredRevenue,
  kDeposits,
  kDebtCurrent,
  kDebtNoncurrent,
  kDebt,
  kTaxLiabilities,
  kCurrentLiabilities,
  kNoncurrentLiabilities,
  kLiabilities,
  kAoci,
  kRetainedEarnings,
  kEquity,
  kSharesOutstanding,
  // cash flow
  kOperatingCash,
  kDepreciation,
  kShareCompensation,
  kCapex,
  kBusinessAcquisitions,
  kInvestmentAcquisitions,
  kInvestingCash,
  kDebtIssuance,
  kEquityIssuance,
  kDividendsPaid,
  kFinancingCash,
  kFxEffect,
  kNetChangeInCash,
  kItemCount
};

struct ItemInfo {
  std::string_view column;
  std::string_view vendor;  // the SF1 indicator
  Statement statement;
  ItemClass item_class;
  SignConvention sign;
  bool subtotal;
};

constexpr auto kInc = Statement::kIncome;
constexpr auto kBal = Statement::kBalanceSheet;
constexpr auto kCfs = Statement::kCashFlow;
constexpr auto kFlow = ItemClass::kFlow;
constexpr auto kStock = ItemClass::kStock;
constexpr auto kShares = ItemClass::kShareCount;
constexpr auto kIn = SignConvention::kPositiveIsInflow;
constexpr auto kOut = SignConvention::kPositiveIsOutflow;
constexpr auto kAny = SignConvention::kSigned;

constexpr std::array<ItemInfo, kItemCount> kItems{{
    {"revenue", "revenue", kInc, kFlow, kIn, false},
    {"cost_of_revenue", "cor", kInc, kFlow, kOut, false},
    {"gross_profit", "gp", kInc, kFlow, kAny, true},
    {"research_and_development", "rnd", kInc, kFlow, kOut, false},
    {"selling_general_and_administrative", "sgna", kInc, kFlow, kOut, false},
    {"operating_expenses", "opex", kInc, kFlow, kOut, true},
    {"operating_income", "opinc", kInc, kFlow, kAny, true},
    {"interest_expense", "intexp", kInc, kFlow, kOut, false},
    {"ebit", "ebit", kInc, kFlow, kAny, true},
    {"income_before_tax", "ebt", kInc, kFlow, kAny, true},
    {"income_tax_expense", "taxexp", kInc, kFlow, kOut, false},
    {"consolidated_income", "consolinc", kInc, kFlow, kAny, true},
    {"net_income_noncontrolling", "netincnci", kInc, kFlow, kAny, false},
    {"net_income_discontinued", "netincdis", kInc, kFlow, kAny, false},
    {"net_income", "netinc", kInc, kFlow, kAny, true},
    {"preferred_dividends", "prefdivis", kInc, kFlow, kOut, false},
    {"net_income_common", "netinccmn", kInc, kFlow, kAny, true},
    {"weighted_average_shares", "shareswa", kInc, kShares, kAny, false},
    {"weighted_average_shares_diluted", "shareswadil", kInc, kShares, kAny, false},
    {"cash_and_equivalents", "cashneq", kBal, kStock, kAny, false},
    {"investments_current", "investmentsc", kBal, kStock, kAny, false},
    {"investments_noncurrent", "investmentsnc", kBal, kStock, kAny, false},
    {"investments_total", "investments", kBal, kStock, kAny, true},
    {"receivables", "receivables", kBal, kStock, kAny, false},
    {"inventory", "inventory", kBal, kStock, kAny, false},
    {"tax_assets", "taxassets", kBal, kStock, kAny, false},
    {"total_current_assets", "assetsc", kBal, kStock, kAny, true},
    {"property_plant_equipment_net", "ppnenet", kBal, kStock, kAny, false},
    {"goodwill_and_intangibles", "intangibles", kBal, kStock, kAny, false},
    {"total_noncurrent_assets", "assetsnc", kBal, kStock, kAny, true},
    {"total_assets", "assets", kBal, kStock, kAny, true},
    {"payables", "payables", kBal, kStock, kAny, false},
    {"deferred_revenue", "deferredrev", kBal, kStock, kAny, false},
    {"deposit_liabilities", "deposits", kBal, kStock, kAny, false},
    {"debt_current", "debtc", kBal, kStock, kAny, false},
    {"debt_noncurrent", "debtnc", kBal, kStock, kAny, false},
    {"total_debt", "debt", kBal, kStock, kAny, true},
    {"tax_liabilities", "taxliabilities", kBal, kStock, kAny, false},
    {"total_current_liabilities", "liabilitiesc", kBal, kStock, kAny, true},
    {"total_noncurrent_liabilities", "liabilitiesnc", kBal, kStock, kAny, true},
    {"total_liabilities", "liabilities", kBal, kStock, kAny, true},
    {"accumulated_other_comprehensive_income", "accoci", kBal, kStock, kAny, false},
    {"retained_earnings", "retearn", kBal, kStock, kAny, false},
    {"total_equity_parent", "equity", kBal, kStock, kAny, true},
    {"shares_outstanding", "sharesbas", kBal, kShares, kAny, false},
    {"net_cash_operating", "ncfo", kCfs, kFlow, kIn, true},
    {"depreciation_amortization", "depamor", kCfs, kFlow, kIn, false},
    {"share_based_compensation", "sbcomp", kCfs, kFlow, kIn, false},
    {"capital_expenditure", "capex", kCfs, kFlow, kIn, false},
    {"net_cash_business_acquisitions", "ncfbus", kCfs, kFlow, kIn, false},
    {"net_cash_investment_acquisitions", "ncfinv", kCfs, kFlow, kIn, false},
    {"net_cash_investing", "ncfi", kCfs, kFlow, kIn, true},
    {"net_debt_issuance", "ncfdebt", kCfs, kFlow, kIn, false},
    {"net_equity_issuance", "ncfcommon", kCfs, kFlow, kIn, false},
    {"dividends_paid", "ncfdiv", kCfs, kFlow, kIn, false},
    {"net_cash_financing", "ncff", kCfs, kFlow, kIn, true},
    {"fx_effect_on_cash", "ncfx", kCfs, kFlow, kIn, false},
    {"net_change_in_cash", "ncf", kCfs, kFlow, kIn, true},
}};

const ItemInfo& info(std::size_t item) { return kItems.at(item); }
bool is_money(std::size_t item) { return info(item).item_class != ItemClass::kShareCount; }

using Values = std::array<double, kItemCount>;

Values blank() {
  Values values{};
  values.fill(kNull);
  return values;
}

// The identities the generator keeps, as tools/synth/statements.toml names them: a
// total and the signed terms it is the sum of.
struct Equation {
  std::string_view name;
  Item total;
  std::vector<std::pair<Item, int>> terms;
};

const std::vector<Equation>& equations() {
  static const std::vector<Equation> kList{
      {"balance_sheet", kAssets, {{kLiabilities, 1}, {kEquity, 1}}},
      {"assets_split", kAssets, {{kCurrentAssets, 1}, {kNoncurrentAssets, 1}}},
      {"liabilities_split", kLiabilities, {{kCurrentLiabilities, 1}, {kNoncurrentLiabilities, 1}}},
      {"debt_split", kDebt, {{kDebtCurrent, 1}, {kDebtNoncurrent, 1}}},
      {"investments_split", kInvestments, {{kInvestmentsCurrent, 1}, {kInvestmentsNoncurrent, 1}}},
      {"gross_profit", kGrossProfit, {{kRevenue, 1}, {kCostOfRevenue, -1}}},
      {"operating_income", kOperatingIncome, {{kGrossProfit, 1}, {kOperatingExpenses, -1}}},
      {"ebit", kEbit, {{kIncomeBeforeTax, 1}, {kInterestExpense, 1}}},
      {"net_income", kNetIncome, {{kConsolidatedIncome, 1}, {kNciIncome, -1}}},
      {"net_income_common", kNetIncomeCommon, {{kNetIncome, 1}, {kPreferredDividends, -1}}},
      {"cash_flow",
       kNetChangeInCash,
       {{kOperatingCash, 1}, {kInvestingCash, 1}, {kFinancingCash, 1}, {kFxEffect, 1}}},
  };
  return kList;
}

// ---------------------------------------------------------------------------
// Companies

enum class Kind : std::uint8_t { kIndustrial, kBank, kPreRevenue };

// What happens to a company in a quarter, decided before it is simulated.
enum class Event : std::uint8_t { kNone, kAcquisition, kWritedown };

struct Quarter {
  Date end;
  int fiscal_year = 0;
  int number = 1;         // its place in its fiscal year, from 1
  bool year_end = false;  // its fiscal year's last quarter: filed with the 10-K
};

struct Split {
  Date ex;
  int numerator = 2;  // new shares for `denominator` old
  int denominator = 1;
  bool on_file = true;
  [[nodiscard]] double ratio() const { return static_cast<double>(numerator) / denominator; }
};

// A row of statements.csv: one version of one statement of one period.
struct Row {
  Statement statement = Statement::kIncome;
  PeriodType type = PeriodType::kQuarter;
  std::size_t quarter = 0;  // its quarter, or its fiscal year's last
  int version = 1;
  Date known_from;
  KnowledgeSource source = KnowledgeSource::kFiled;
  std::optional<Date> filing_date;
  Values values = blank();  // NULL off its statement
  bool removed = false;     // a missing quarter's: not written
  int copies = 1;           // a duplicated row's: written twice
  // A version chain's fault: known_to set apart from the next version's
  // known_from, or left open.
  std::optional<Date> known_to;
  bool open = false;
};

struct Company {
  std::string id;
  Kind kind = Kind::kIndustrial;
  std::string sector;
  std::string industry;
  std::string exchange = "XNYS";
  std::string currency = "USD";  // the reporting currency; every company trades in USD
  int fye_month = 12;
  bool saturday = false;  // a 52/53-week year: periods end on the month's last Saturday
  int change_year = 0;    // the last fiscal year before a change of fiscal year, or 0
  int new_month = 12;     // the fiscal year end after it
  int first_year = 2003;
  bool delisted = false;
  int last_year = 0;  // a delisted company's last fiscal year
  double unit = 1000.0;
  double revenue = 1e8;  // the first quarter's revenue, before seasonality
  bool nci = false;
  bool temporary_equity = false;
  bool restricted_cash = false;
  bool writedown_ready = false;  // holds enough goodwill for a write-down
  int q_deadline = 45;
  int k_deadline = 90;
  int q_lag = 40;
  int k_lag = 80;

  std::vector<Quarter> quarters;
  std::vector<Event> events;  // by quarter
  std::vector<bool> busy;     // by quarter: taken by a fault or a hard negative
  // By quarter, as the company stands, with share counts in its first terms
  // (before any split).
  std::vector<Values> values;
  std::vector<double> shares;  // outstanding at each quarter end, first terms
  double shares0 = 1e7;
  std::vector<Date> filed;  // by quarter: when the 10-Q (or the 10-K) became known
  std::vector<KnowledgeSource> source;
  Date day0;                  // the first day of `mcap`
  std::vector<double> mcap;   // the market cap, daily from day0
  std::vector<Split> splits;  // every split, on file or not
  std::vector<Row> rows;
  Date first_trade;
  Date last_trade;
};

double factor_at(const Company& c, Date date) {
  double f = 1.0;
  for (const Split& s : c.splits) {
    if (s.ex <= date) {
      f *= s.ratio();
    }
  }
  return f;
}

double shares_at(const Company& c, Date date) {
  double shares = c.shares0;
  for (std::size_t t = 0; t < c.quarters.size() && c.quarters[t].end <= date; ++t) {
    shares = c.shares[t];
  }
  return shares;
}

double mcap_at(const Company& c, Date date) {
  const auto i = std::clamp<std::int64_t>(date.days() - c.day0.days(), 0,
                                          static_cast<std::int64_t>(c.mcap.size()) - 1);
  return c.mcap[static_cast<std::size_t>(i)];
}

// The raw close on `date`: the market cap over the shares in that day's terms.
double close_at(const Company& c, Date date) {
  const double price = mcap_at(c, date) / (shares_at(c, date) * factor_at(c, date));
  return std::max(0.01, std::round(price * 100.0) / 100.0);
}

// The last day of a month, or its last Saturday.
Date month_end(int year, int month, bool saturday) {
  Date d = month == 12 ? Date::from_ymd(year + 1, 1, 1) : Date::from_ymd(year, month + 1, 1);
  d = plus_days(d, -1);
  while (saturday && d.weekday() != 5) {
    d = plus_days(d, -1);
  }
  return d;
}

// A fiscal year's quarters, named by the year it ends in.
void add_year(Company& c, int year, int month, int count, int first_month) {
  for (int j = 1; j <= count; ++j) {
    int m = first_month + 3 * (j - 1);
    int y = year;
    if (m > 12) {
      m -= 12;
    }
    if (m > month) {
      y = year - 1;
    }
    c.quarters.push_back({month_end(y, m, c.saturday), year, j, j == count});
  }
}

void plan_quarters(Company& c) {
  int month = c.fye_month;
  const int last = c.delisted ? c.last_year : 2026;
  for (int year = c.first_year; year <= last; ++year) {
    if (c.change_year != 0 && year == c.change_year + 1) {
      // The transition: from the old year end to the new, a short fiscal year.
      const int count = ((c.new_month - month + 12) % 12) / 3;
      add_year(c, year, c.new_month, count, month % 12 + 3);
      month = c.new_month;
      continue;
    }
    add_year(c, year, month, 4, (month + 3 - 1) % 12 + 1);
  }
  std::erase_if(c.quarters, [](const Quarter& q) { return q.end > newest_end(); });
}

// The first and last quarters of quarter t's fiscal year.
std::size_t year_first(const Company& c, std::size_t t) {
  return t + 1 - static_cast<std::size_t>(c.quarters[t].number);
}

std::size_t year_last(const Company& c, std::size_t t) {
  while (!c.quarters[t].year_end && t + 1 < c.quarters.size()) {
    ++t;
  }
  return t;
}

Row* find_row(Company& c, Statement s, PeriodType type, std::size_t t, int version) {
  for (Row& row : c.rows) {
    if (row.statement == s && row.type == type && row.quarter == t && row.version == version) {
      return &row;
    }
  }
  return nullptr;
}

// A row that must be there: every quarter has its version 1 rows.
Row& row_of(Company& c, Statement s, PeriodType type, std::size_t t, int version) {
  Row* row = find_row(c, s, type, t, version);
  if (row == nullptr) {
    throw std::logic_error(c.id + ": no row for quarter " + std::to_string(t));
  }
  return *row;
}

// Takes the fiscal years of quarters [from, to], and a quarter either side, if
// none of them is taken.
bool reserve(Company& c, std::size_t from, std::size_t to) {
  from = year_first(c, from);
  to = year_last(c, to);
  from = from == 0 ? 0 : from - 1;
  to = std::min(to + 1, c.quarters.size() - 1);
  for (std::size_t t = from; t <= to; ++t) {
    if (c.busy[t]) {
      return false;
    }
  }
  for (std::size_t t = from; t <= to; ++t) {
    c.busy[t] = true;
  }
  return true;
}

// A fiscal year's values: its flows summed over its quarters, its stocks at its
// end, and its weighted shares the mean of its quarters'.
Values annual_values(const Company& c, std::size_t t) {
  Values out = c.values[t];
  const std::size_t from = year_first(c, t);
  double weighted = 0.0;
  double diluted = 0.0;
  for (std::size_t item = 0; item < kItemCount; ++item) {
    if (info(item).item_class != ItemClass::kFlow) {
      continue;
    }
    double sum = 0.0;
    for (std::size_t i = from; i <= t; ++i) {
      sum += c.values[i].at(item);
    }
    out.at(item) = sum;
  }
  for (std::size_t i = from; i <= t; ++i) {
    weighted += c.values[i][kSharesWeighted];
    diluted += c.values[i][kSharesDiluted];
  }
  const auto count = static_cast<double>(t - from + 1);
  out[kSharesWeighted] = weighted / count;
  out[kSharesDiluted] = diluted / count;
  return out;
}

// A row of one statement: the company's values, with its counts in the terms of
// `known`.
Row make_row(const Company& c, Statement s, PeriodType type, std::size_t t, int version, Date known,
             KnowledgeSource source, std::optional<Date> filing) {
  Row row;
  row.statement = s;
  row.type = type;
  row.quarter = t;
  row.version = version;
  row.known_from = known;
  row.source = source;
  row.filing_date = filing;
  const Values all = type == PeriodType::kQuarter ? c.values[t] : annual_values(c, t);
  const double f = factor_at(c, known);
  for (std::size_t item = 0; item < kItemCount; ++item) {
    if (info(item).statement != s) {
      continue;
    }
    double value = all.at(item);
    if (!is_money(item) && !std::isnan(value)) {
      value = std::round(value * f);  // a count, in its filing's share terms
    }
    row.values.at(item) = value;
  }
  if (version > 1) {
    row.values[kSharesOutstanding] = kNull;  // the cover's count: first-reported rows only
  }
  return row;
}

// Each quarter's first versions: three statements, and the year with its last.
void first_versions(Company& c) {
  for (std::size_t t = 0; t < c.quarters.size(); ++t) {
    const std::optional<Date> filing =
        c.source[t] == KnowledgeSource::kFiled ? std::optional<Date>{c.filed[t]} : std::nullopt;
    for (const Statement s : {kInc, kBal, kCfs}) {
      c.rows.push_back(make_row(c, s, PeriodType::kQuarter, t, 1, c.filed[t], c.source[t], filing));
      if (c.quarters[t].year_end) {
        c.rows.push_back(
            make_row(c, s, PeriodType::kAnnual, t, 1, c.filed[t], c.source[t], filing));
      }
    }
  }
}

// DQ903's tolerance for an equation on a row: the larger of 0.5% of its largest
// term and 1M (tools/synth/statements.toml's identity_share and tie_floor).
double identity_tolerance(const Row& row, const Equation& eq) {
  double scale = std::abs(row.values.at(eq.total));
  for (const auto& [term, sign] : eq.terms) {
    scale = std::max(scale, std::abs(row.values.at(term)));
  }
  return std::max(0.005 * scale, 1e6);
}

// Whether an equation fails on a row beyond that tolerance; false where a term is
// NULL, as DQ903 does not judge it then.
bool identity_breaks_on(const Row& row, const Equation& eq) {
  double gap = row.values.at(eq.total);
  if (std::isnan(gap)) {
    return false;
  }
  for (const auto& [term, sign] : eq.terms) {
    const double x = row.values.at(term);
    if (std::isnan(x)) {
      return false;
    }
    gap -= sign * x;
  }
  return std::abs(gap) > identity_tolerance(row, eq);
}

// ---------------------------------------------------------------------------
// Labels

struct Label {
  Label(std::string id, Date from, Date to, std::string what, std::string code, std::string prefix)
      : series(std::move(id)),
        first(from),
        last(to),
        kind(std::move(what)),
        expect(std::move(code)),
        codes(std::move(prefix)) {}

  std::string series;
  Date first;
  Date last;
  std::string kind;
  std::string expect;    // a fault's code
  std::string codes;     // a hard negative's code prefix
  bool context = false;  // a context gap, not a data error
  std::optional<Statement> statement;
  std::optional<PeriodType> type;
  std::optional<int> version;
  std::string item;
  std::string identity;  // a structural gap: the identity or tie it breaks
};

// Whether a label covers one of its company's rows: its dates, and its statement,
// period type and version where it names them.
bool covers(const Company& c, const Label& label, const Row& row) {
  const Date end = c.quarters[row.quarter].end;
  return !row.removed && label.series == c.id && label.first <= end && end <= label.last &&
         (!label.statement || *label.statement == row.statement) &&
         (!label.type || *label.type == row.type) &&
         (!label.version || *label.version == row.version);
}

// An equation's gap on a set of values, or nullopt where a term is NULL.
std::optional<double> equation_gap(const Values& values, const Equation& eq) {
  double gap = values.at(eq.total);
  for (const auto& [term, sign] : eq.terms) {
    gap -= sign * values.at(term);
  }
  if (std::isnan(gap)) {
    return std::nullopt;
  }
  return gap;
}

// A fiscal year's last month: December 65% of the time, else September, June or
// March.
int fiscal_year_end(double u) {
  if (u < 0.65) {
    return 12;
  }
  if (u < 0.8) {
    return 9;
  }
  return u < 0.9 ? 6 : 3;
}

// A label's class in dorq's label schema (doc/labels.md).
std::string_view class_of(const Label& l) {
  if (l.expect.empty()) {
    return "market_fact";
  }
  return l.context ? "context_gap" : "data_error";
}

std::string integer_text(double value) {
  if (std::isnan(value)) {
    return "";
  }
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(0);
  out << (std::round(value) + 0.0);
  return out.str();
}

std::string price_text(double value) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(2);
  out << value;
  return out.str();
}

// ---------------------------------------------------------------------------
// Simulating a company

// What a company's simulation draws once.
struct Params {
  double growth = 0.0;  // a year
  double grow_q = 1.0;  // a quarter
  double amp = 0.0;     // seasonality
  int phase = 0;
  // Shares of revenue; a biotech's research and SG&A are shares of its scale.
  double cogs = 0.0;
  double sga = 0.0;
  double research = 0.0;
  double other_opex = 0.0;
  double tax_rate = 0.0;
  double rate = 0.0;          // interest on debt, a quarter
  double deposit_rate = 0.0;  // and on a bank's deposits
  double dso = 0.0;
  double inventory_ratio = 0.0;
  double payables_ratio = 0.0;
  double deferred_ratio = 0.0;
  double depreciation = 0.0;
  double capex_mult = 1.0;
  double sbc = 0.0;
  double payout = 0.0;
  double buyback = 0.0;
  double debt_current_share = 0.0;
  double nci_share = 0.0;
  double preferred = 0.0;
  double dilution = 0.0;
  double other_ca_ratio = 0.0;
  double other_nca_ratio = 0.0;
  double other_cl_ratio = 0.0;
  double other_ncl_ratio = 0.0;
  std::size_t revenue_from = 0;  // a biotech's first quarter of sales
};

// A company's balances at a quarter's end.
struct Books {
  double cash = 0.0;
  double invc = 0.0;
  double invnc = 0.0;
  double loans = 0.0;  // a bank's investments
  double deposits = 0.0;
  double ppe = 0.0;
  double goodwill = 0.0;
  double tax_assets = 0.0;
  double tax_liabilities = 0.0;
  double debt = 0.0;
  double nci_equity = 0.0;
  double temporary = 0.0;
  double restricted = 0.0;
  double aoci = 0.0;
  double retained = 0.0;
  double receivables = 0.0;
  double inventory = 0.0;
  double payables = 0.0;
  double deferred = 0.0;
  double other_ca = 0.0;
  double other_cl = 0.0;
};

// What a quarter's cash flow did with the shares and the shareholders.
struct Trades {
  double issuance = 0.0;
  double repurchase = 0.0;
  double compensation = 0.0;  // stock-based
  double dividends = 0.0;
};

// A company's quarters, as it stands: every identity holds, and the cash flow
// moves the balance sheet's cash (restricted cash aside, from 2018).
class Simulation {
 public:
  Simulation(Rng& rng, Company& c)
      : rng_(rng), c_(c), bank_(c.kind == Kind::kBank), pre_(c.kind == Kind::kPreRevenue) {}

  void run() {
    draw_parameters();
    open_books();
    plan_scales();
    plan_market_cap();
    c_.values.assign(n(), blank());
    c_.shares.assign(n(), 0.0);
    for (std::size_t t = 0; t < n(); ++t) {
      quarter(t);
    }
  }

 private:
  [[nodiscard]] std::size_t n() const { return c_.quarters.size(); }
  [[nodiscard]] double r(double v) const { return std::round(v / c_.unit) * c_.unit + 0.0; }
  double lognormal(double sd) { return std::exp(sd * rng_.normal()); }

  void draw_parameters();
  void open_books();
  void plan_scales();
  void plan_market_cap();
  void quarter(std::size_t t);
  void income(std::size_t t, Values& v, double impairment);
  [[nodiscard]] Books roll(std::size_t t, const Values& v);
  Trades cash_flow(std::size_t t, Values& v, Books& next, double price, double impairment);
  void close_books(std::size_t t, Values& v, Books& next, double price, const Trades& trades);
  void count_shares(std::size_t t, Values& v, const Trades& trades);

  Rng& rng_;
  Company& c_;
  bool bank_;
  bool pre_;
  Params p_;
  Books b_;
  double level_ = 0.0;  // the scale: quarterly revenue, or a biotech's research budget
  std::vector<double> scales_;
  std::vector<double> steps_;  // an acquisition's step in the scale, from the next quarter
  double shares_ = 0.0;
};

void Simulation::draw_parameters() {
  Params& p = p_;
  p.revenue_from = n();
  switch (c_.kind) {
    case Kind::kIndustrial:
      level_ = c_.revenue;
      p.growth = rng_.uniform(-0.03, 0.15);
      p.amp = rng_.uniform(0.0, 0.25);
      p.cogs = rng_.uniform(0.35, 0.72);
      p.sga = rng_.uniform(0.08, 0.22);
      p.research = rng_.chance(0.45) ? rng_.uniform(0.02, 0.14) : 0.0;
      p.other_opex = rng_.uniform(0.03, 0.12);
      p.tax_rate = rng_.uniform(0.15, 0.3);
      p.dso = rng_.uniform(0.4, 1.0);
      p.deferred_ratio = rng_.chance(0.5) ? rng_.uniform(0.05, 0.35) : 0.0;
      p.payout = rng_.chance(0.55) ? rng_.uniform(0.15, 0.5) : 0.0;
      p.buyback = rng_.chance(0.4) ? rng_.uniform(0.05, 0.3) : 0.0;
      p.preferred = rng_.chance(0.1) ? level_ * rng_.uniform(0.002, 0.01) : 0.0;
      break;
    case Kind::kBank:
      level_ = c_.revenue;
      p.growth = rng_.uniform(-0.03, 0.15);
      p.sga = rng_.uniform(0.35, 0.55);
      p.other_opex = rng_.uniform(0.0, 0.05);
      p.tax_rate = rng_.uniform(0.15, 0.3);
      p.dso = rng_.uniform(0.2, 0.4);
      p.payout = rng_.uniform(0.3, 0.6);
      p.buyback = rng_.chance(0.4) ? rng_.uniform(0.05, 0.3) : 0.0;
      p.preferred = rng_.chance(0.1) ? level_ * rng_.uniform(0.002, 0.01) : 0.0;
      break;
    case Kind::kPreRevenue:
      level_ = rng_.log_uniform(5e6, 4e7);
      p.growth = 0.125;
      p.cogs = 0.3;
      p.sga = rng_.uniform(0.25, 0.5);
      p.research = 1.0;
      p.dso = rng_.uniform(0.4, 1.0);
      if (rng_.chance(0.4)) {  // it starts selling, some years in
        p.revenue_from = static_cast<std::size_t>(rng_.integer(12, static_cast<int>(n())));
      }
      break;
  }
  p.grow_q = std::pow(1.0 + p.growth, 0.25);
  p.phase = rng_.integer(0, 3);
  p.rate = rng_.uniform(0.008, 0.018);
  p.deposit_rate = rng_.uniform(0.002, 0.006);
  p.inventory_ratio = rng_.uniform(0.3, 1.4);
  p.payables_ratio = rng_.uniform(0.3, 0.8);
  p.depreciation = rng_.uniform(0.015, 0.03);
  p.capex_mult = rng_.uniform(1.0, 1.4);
  p.sbc = rng_.chance(0.7) ? rng_.uniform(0.005, 0.04) : 0.0;
  p.debt_current_share = rng_.uniform(0.05, 0.25);
  p.nci_share = c_.nci ? rng_.uniform(0.03, 0.12) : 0.0;
  p.dilution = rng_.uniform(0.005, 0.03);
  p.other_ca_ratio = rng_.uniform(0.05, 0.25);
  p.other_nca_ratio = rng_.uniform(0.05, 0.3);
  p.other_cl_ratio = rng_.uniform(0.1, 0.3);
  p.other_ncl_ratio = rng_.uniform(0.05, 0.4);
}

void Simulation::open_books() {
  Books& b = b_;
  const double level = level_;
  b.cash = r(level * rng_.uniform(0.3, 1.5) * (pre_ ? 6.0 : 1.0));
  b.invc = rng_.chance(0.6) ? r(level * rng_.uniform(0.05, 0.6) * (pre_ ? 4.0 : 1.0)) : 0.0;
  b.loans = r(level * rng_.uniform(20.0, 35.0));
  b.deposits = r(b.loans * rng_.uniform(0.75, 0.9));
  b.receivables = r(level * p_.dso);
  b.inventory = r(level * p_.cogs * p_.inventory_ratio);
  b.payables = r(level * (pre_ ? 0.25 : p_.cogs * p_.payables_ratio + 0.05));
  b.deferred = r(level * p_.deferred_ratio);
  b.other_ca = r(level * p_.other_ca_ratio);
  b.other_cl = r(level * p_.other_cl_ratio);
  b.nci_equity = c_.nci ? r(level * rng_.uniform(0.2, 0.8)) : 0.0;
  b.temporary = c_.temporary_equity ? r(level * rng_.uniform(0.1, 0.4)) : 0.0;
  // Restricted cash: a few per cent of what the company holds (Apple's is about 4%).
  b.restricted = c_.restricted_cash ? r(level * rng_.uniform(0.01, 0.04)) : 0.0;
  b.aoci = r(level * 0.05 * rng_.normal());
  if (pre_) {  // no goodwill, tax balances, long-term investments or debt
    b.ppe = r(level * rng_.uniform(0.2, 0.6));
    b.retained = r(-level * rng_.uniform(1.0, 4.0));
    return;
  }
  b.invnc = rng_.chance(0.5) ? r(level * rng_.uniform(0.05, 1.0)) : 0.0;
  b.ppe = r(level * rng_.uniform(0.5, 3.5));
  b.goodwill = r(level * (c_.writedown_ready ? rng_.uniform(1.0, 3.0) : rng_.uniform(0.0, 2.0)));
  b.tax_assets = r(level * rng_.uniform(0.0, 0.1));
  b.tax_liabilities = r(level * rng_.uniform(0.0, 0.15));
  b.debt = r(level * (bank_ ? rng_.uniform(1.0, 4.0) : rng_.uniform(0.0, 3.0)));
  b.retained = r(level * rng_.uniform(1.0, 6.0));
}

// The scale by quarter: it grows, and an acquisition steps it for good.
void Simulation::plan_scales() {
  scales_.assign(n(), 0.0);
  steps_.assign(n(), 1.0);
  double level = level_;
  double step = 1.0;
  for (std::size_t t = 0; t < n(); ++t) {
    level *= p_.grow_q * std::exp(0.01 * rng_.normal());
    scales_[t] = level * step;
    if (c_.events[t] == Event::kAcquisition) {
      steps_[t] = rng_.uniform(1.15, 1.5);
      step *= steps_[t];
    }
  }
}

// The market cap, daily: the scale at a multiple that wanders and reverts.
void Simulation::plan_market_cap() {
  c_.day0 = plus_days(c_.quarters.front().end, -rng_.integer(100, 160));
  c_.first_trade = plus_days(c_.day0, rng_.integer(0, 30));
  double base = std::log(rng_.uniform(1.0, 4.0));
  double sigma = 0.35;
  if (pre_) {
    base = std::log(rng_.uniform(4.0, 20.0));
    sigma = 0.6;
  } else if (bank_) {
    base = std::log(rng_.uniform(2.0, 4.0));
    sigma = 0.25;
  }
  const int days = last_session().days() - c_.day0.days() + 1;
  c_.mcap.resize(static_cast<std::size_t>(days));
  double multiple = base;
  std::size_t quarter = 0;
  for (std::size_t d = 0; d < c_.mcap.size(); ++d) {
    const Date day = plus_days(c_.day0, static_cast<int>(d));
    while (quarter + 1 < n() && c_.quarters[quarter].end < day) {
      ++quarter;
    }
    c_.mcap[d] = 4.0 * scales_[quarter] * std::exp(multiple);
    multiple += 0.7 * (base - multiple) / 365.0 + sigma / std::sqrt(365.0) * rng_.normal();
  }
  const double price0 = pre_ ? rng_.uniform(3.0, 25.0) : rng_.uniform(15.0, 90.0);
  c_.shares0 = std::round(c_.mcap.front() / price0);
  shares_ = c_.shares0;
}

void Simulation::quarter(std::size_t t) {
  Values& v = c_.values[t];
  double price = 0.0;  // an acquisition's
  double impairment = 0.0;
  if (c_.events[t] == Event::kAcquisition) {
    price = r(scales_[t] * (steps_[t] - 1.0) * rng_.uniform(6.0, 14.0));
  }
  if (c_.events[t] == Event::kWritedown) {
    impairment = r(b_.goodwill * rng_.uniform(0.5, 0.9));
  }
  income(t, v, impairment);
  Books next = roll(t, v);
  const Trades trades = cash_flow(t, v, next, price, impairment);
  close_books(t, v, next, price, trades);
  count_shares(t, v, trades);
  b_ = next;
}

void Simulation::income(std::size_t t, Values& v, double impairment) {
  const double scale = scales_[t];
  double revenue = 0.0;
  if (!pre_) {
    const double season =
        1.0 + p_.amp * std::cos(2.0 * std::numbers::pi *
                                static_cast<double>(c_.quarters[t].number - 1 - p_.phase) / 4.0);
    revenue = r(scale * season * lognormal(0.03));
  } else if (t >= p_.revenue_from) {
    revenue = r(scale * 0.1 * std::pow(1.15, static_cast<double>(t - p_.revenue_from)));
  }
  const double base = pre_ ? scale : revenue;  // what research and SG&A are shares of
  const double cost = r(revenue * p_.cogs * lognormal(0.01));
  const double research = r(base * p_.research * lognormal(0.03));
  const double sga = r(base * p_.sga * lognormal(0.03));
  const double other = r(revenue * p_.other_opex) + impairment;
  const double interest = r(b_.debt * p_.rate + (bank_ ? b_.deposits * p_.deposit_rate : 0.0));
  const double other_income = pre_ ? r(b_.cash * 0.004) : r(revenue * 0.004 * rng_.normal());
  v[kRevenue] = revenue;
  v[kCostOfRevenue] = cost;
  v[kGrossProfit] = revenue - cost;
  v[kResearch] = research;
  v[kSga] = sga;
  v[kOperatingExpenses] = sga + research + other;
  v[kOperatingIncome] = v[kGrossProfit] - v[kOperatingExpenses];
  v[kInterestExpense] = interest;
  v[kIncomeBeforeTax] = v[kOperatingIncome] - interest + other_income;
  v[kEbit] = v[kIncomeBeforeTax] + interest;
  const double ebt = v[kIncomeBeforeTax];
  v[kIncomeTax] = r(ebt > 0 ? ebt * p_.tax_rate : ebt * p_.tax_rate * 0.5);
  v[kDiscontinued] = 0.0;
  v[kConsolidatedIncome] = ebt - v[kIncomeTax] + v[kDiscontinued];
  v[kNciIncome] = r(v[kConsolidatedIncome] * p_.nci_share);
  v[kNetIncome] = v[kConsolidatedIncome] - v[kNciIncome];
  v[kPreferredDividends] = r(p_.preferred);
  v[kNetIncomeCommon] = v[kNetIncome] - v[kPreferredDividends];
}

// Working capital and the other balances at the quarter's end, cash and debt
// aside.
Books Simulation::roll(std::size_t t, const Values& v) {
  const double scale = scales_[t];
  const double revenue = v[kRevenue];
  const double cost = v[kCostOfRevenue];
  Books next = b_;
  next.receivables = r(revenue * p_.dso * lognormal(0.05));
  next.inventory = r(cost * p_.inventory_ratio * lognormal(0.05));
  const double owed =
      pre_ ? v[kOperatingExpenses] * 0.25 : cost * p_.payables_ratio + revenue * 0.05;
  next.payables = r(owed * lognormal(0.05));
  next.deferred = r(revenue * p_.deferred_ratio * lognormal(0.05));
  if (!pre_) {
    next.tax_assets = r(std::max(0.0, b_.tax_assets + scale * 0.01 * rng_.normal()));
    next.tax_liabilities = r(std::max(0.0, b_.tax_liabilities + scale * 0.01 * rng_.normal()));
  }
  next.invc = r(b_.invc * lognormal(0.08));
  next.invnc = r(b_.invnc * lognormal(0.08));
  next.loans = r(b_.loans * p_.grow_q * lognormal(0.02));
  next.deposits = r(b_.deposits * p_.grow_q * lognormal(0.02));
  next.restricted = r(b_.restricted * lognormal(0.05));
  next.other_ca = r(scale * p_.other_ca_ratio * lognormal(0.05));
  next.other_cl = r(scale * p_.other_cl_ratio * lognormal(0.05));
  return next;
}

// The cash flow, and the cash, debt, property and goodwill it leaves.
Trades Simulation::cash_flow(std::size_t t, Values& v, Books& next, double price,
                             double impairment) {
  const Date end = c_.quarters[t].end;
  const double scale = scales_[t];
  const double revenue = v[kRevenue];
  const Books& b = b_;
  Trades trades;
  const double amortization = r(b.goodwill * 0.008);
  const double dep = r(b.ppe * p_.depreciation);
  const double capex =
      -r(dep * p_.capex_mult * std::max(1.0, p_.grow_q * p_.grow_q) * lognormal(0.1));
  trades.compensation = r((pre_ ? scale : revenue) * p_.sbc);
  const double working = -(next.receivables - b.receivables) - (next.inventory - b.inventory) +
                         (next.payables - b.payables) + (next.deferred - b.deferred) -
                         (next.tax_assets - b.tax_assets) +
                         (next.tax_liabilities - b.tax_liabilities) - (next.other_ca - b.other_ca) +
                         (next.other_cl - b.other_cl);
  const double operating = v[kConsolidatedIncome] + amortization + dep + trades.compensation +
                           impairment + working + r(revenue * 0.01 * rng_.normal());
  // From 2018 the cash flow's cash includes restricted cash; before, a change in
  // it was an investing flow.
  const bool restricted_in_cash = c_.restricted_cash && end >= restricted_cash_rule();
  const double restricted_change = next.restricted - b.restricted;
  const double investment_change =
      bank_ ? next.loans - b.loans : (next.invc + next.invnc) - (b.invc + b.invnc);
  const double investing =
      capex - price - investment_change - (restricted_in_cash ? 0.0 : restricted_change);
  trades.dividends = r(std::max(0.0, v[kNetIncome]) * p_.payout);
  trades.repurchase = r(std::max(0.0, v[kNetIncome]) * p_.buyback * rng_.uniform(0.0, 2.0));
  double debt_change = std::max(-b.debt, pre_ ? 0.0 : r(b.debt * 0.03 * rng_.normal()) + price);
  const double fx = c_.currency == "USD" ? 0.0 : r(b.cash * 0.015 * rng_.normal());
  const double deposit_change = bank_ ? next.deposits - b.deposits : 0.0;
  const double preferred = v[kPreferredDividends];
  double cash = b.cash + operating + investing + debt_change - trades.repurchase -
                trades.dividends - preferred + deposit_change + fx -
                (restricted_in_cash ? restricted_change : 0.0);
  // Short of cash, a company borrows, or a biotech sells shares.
  if (const double floor = r(pre_ ? 4.0 * v[kOperatingExpenses] : scale * 0.15); cash < floor) {
    const double need = r(2.0 * floor - cash);
    (pre_ ? trades.issuance : debt_change) += need;
    cash += need;
  }
  // An industrial's cash beyond what the business needs goes back to its
  // shareholders, a tenth of the market cap a quarter at most.
  if (const double ceiling = scale * 3.0; c_.kind == Kind::kIndustrial && cash > ceiling) {
    const double excess = r(std::min(cash - ceiling, 0.1 * mcap_at(c_, end)));
    trades.repurchase += excess;
    cash -= excess;
  }
  v[kOperatingCash] = operating;
  v[kDepreciation] = amortization + dep;
  v[kShareCompensation] = trades.compensation;
  v[kCapex] = capex;
  v[kBusinessAcquisitions] = -price;
  v[kInvestmentAcquisitions] = -investment_change;
  v[kInvestingCash] = investing;
  v[kDebtIssuance] = debt_change;
  v[kEquityIssuance] = trades.issuance - trades.repurchase;
  v[kDividendsPaid] = -(trades.dividends + preferred);
  v[kFinancingCash] = v[kDebtIssuance] + v[kEquityIssuance] + v[kDividendsPaid] + deposit_change;
  v[kFxEffect] = fx;
  v[kNetChangeInCash] = v[kOperatingCash] + v[kInvestingCash] + v[kFinancingCash] + fx;
  next.cash = cash;
  next.debt = b.debt + debt_change;
  next.ppe = b.ppe - capex - dep + r(price * 0.25);
  next.goodwill = b.goodwill - amortization + r(price * 0.6) - impairment;
  return trades;
}

// The balance sheet at the quarter's end.
void Simulation::close_books(std::size_t t, Values& v, Books& next, double price,
                             const Trades& trades) {
  const double scale = scales_[t];
  next.nci_equity += v[kNciIncome];
  next.retained += v[kNetIncomeCommon] - trades.dividends;
  next.aoci += r(scale * 0.01 * rng_.normal());
  const double other_nca = r(scale * p_.other_nca_ratio) + r(price * 0.15);
  const double other_ncl = r(scale * p_.other_ncl_ratio);
  const double debt_current = r(next.debt * p_.debt_current_share);
  const Books& b = next;
  v[kCash] = b.cash;
  v[kReceivables] = b.receivables;
  v[kPpe] = b.ppe;
  v[kIntangibles] = b.goodwill;
  v[kTaxAssets] = b.tax_assets;
  v[kPayables] = b.payables;
  v[kDeferredRevenue] = b.deferred;
  v[kDebt] = b.debt;
  v[kTaxLiabilities] = b.tax_liabilities;
  v[kAoci] = b.aoci;
  v[kRetainedEarnings] = b.retained;
  if (bank_) {
    // No current and noncurrent split, and no inventory: NULL, not 0.
    v[kInvestments] = b.loans;
    v[kDeposits] = b.deposits;
    v[kAssets] = b.cash + b.loans + b.receivables + b.ppe + b.goodwill + b.tax_assets + b.other_ca +
                 other_nca + b.restricted;
    v[kLiabilities] =
        b.deposits + b.debt + b.payables + b.deferred + b.tax_liabilities + b.other_cl + other_ncl;
  } else {
    v[kInvestmentsCurrent] = b.invc;
    v[kInvestmentsNoncurrent] = b.invnc;
    v[kInvestments] = b.invc + b.invnc;
    v[kInventory] = b.inventory;
    v[kDeposits] = 0.0;
    v[kDebtCurrent] = debt_current;
    v[kDebtNoncurrent] = b.debt - debt_current;
    v[kCurrentAssets] = b.cash + b.invc + b.receivables + b.inventory + b.other_ca + b.restricted;
    v[kNoncurrentAssets] = b.ppe + b.goodwill + b.invnc + b.tax_assets + other_nca;
    v[kAssets] = v[kCurrentAssets] + v[kNoncurrentAssets];
    v[kCurrentLiabilities] = b.payables + b.deferred + debt_current + b.other_cl;
    v[kNoncurrentLiabilities] = (b.debt - debt_current) + b.tax_liabilities + other_ncl;
    v[kLiabilities] = v[kCurrentLiabilities] + v[kNoncurrentLiabilities];
  }
  v[kEquity] = v[kAssets] - v[kLiabilities] - b.nci_equity - b.temporary;
}

// Shares, in the company's first terms: employee plans add a little each quarter,
// and shares are sold and bought back at the day's price.
void Simulation::count_shares(std::size_t t, Values& v, const Trades& trades) {
  const double price = mcap_at(c_, c_.quarters[t].end) / shares_;
  const double before = shares_;
  shares_ = std::max(before * 0.5,
                     before * (1.0015 + 0.002 * rng_.normal()) +
                         (trades.issuance - trades.repurchase + 0.5 * trades.compensation) / price);
  c_.shares[t] = shares_;
  v[kSharesWeighted] = 0.5 * (before + shares_);
  v[kSharesDiluted] = v[kSharesWeighted] * (1.0 + (v[kNetIncome] > 0 ? p_.dilution : 0.0));
  v[kSharesOutstanding] = shares_ * 1.001;
}

// ---------------------------------------------------------------------------
// The universe

class Universe {
 public:
  explicit Universe(std::uint64_t seed) : rng_(seed + 0x5354415445ULL) {}

  void build();
  void write(const std::filesystem::path& dir) const;

 private:
  void write_statements(const std::filesystem::path& dir) const;
  void write_context(const std::filesystem::path& dir) const;  // items, actions, meta
  void write_labels(const std::filesystem::path& dir) const;
  void add_companies();
  void label_fiscal_year_change(Company& c);
  void plan_events(Company& c);
  void simulate(Company& c);
  void plan_splits(Company& c);
  void plan_filings(Company& c);
  void late_filings();
  void restatements(Company& c);
  void faults();
  void whole_series_labels();

  Company& any(Kind kind);
  Company& any();
  std::size_t pick_quarter(Company& c);
  Statement any_statement();
  [[nodiscard]] Date session_on_or_after(Date date) const;
  Date restatement_date(const Company& c, std::size_t year_end, KnowledgeSource& source,
                        std::optional<Date>& filing);
  void label_row(const Company& c, const Row& row, const char* kind, const char* expect,
                 std::string item = {});

  // The faults.
  void unit_errors();
  void sign_flips();
  void bad_values();
  void copied_periods();
  void absent_as_zero();
  void cumulative_quarters();
  void identity_breaks();
  void share_faults();
  void filing_faults();
  void missing_quarters();
  void restated_at_scale();
  void key_faults();
  void impossible_values();
  void mark_impossible();
  void mark_identities();
  void mark_ties();
  Company* company(const std::string& id);

  Rng rng_;
  dorq::Calendar calendar_{dorq::CalendarKind::kXnys};
  std::vector<Company> companies_;
  std::vector<Label> labels_;
};

Date Universe::session_on_or_after(Date date) const {
  while (!calendar_.is_session(date)) {
    date = plus_days(date, 1);
  }
  return date;
}

Company& Universe::any() {
  return companies_[static_cast<std::size_t>(
      rng_.integer(0, static_cast<int>(companies_.size()) - 1))];
}

Company& Universe::any(Kind kind) {
  while (true) {
    Company& c = any();
    if (c.kind == kind) {
      return c;
    }
  }
}

// A quarter for a fault: filed (not estimated), with a year of periods either side;
// or kNoQuarter, which is never one (the first quarters are not picked).
std::size_t Universe::pick_quarter(Company& c) {
  const auto n = static_cast<int>(c.quarters.size());
  if (n < 16) {
    return kNoQuarter;
  }
  const auto t = static_cast<std::size_t>(rng_.integer(5, n - 6));
  return c.source[t] == KnowledgeSource::kFiled ? t : kNoQuarter;
}

// A statement for a fault: the income statement 40% of the time, else the balance
// sheet or the cash flow.
Statement Universe::any_statement() {
  if (rng_.chance(0.4)) {
    return kInc;
  }
  return rng_.chance(0.5) ? kBal : kCfs;
}

void Universe::add_companies() {
  static const std::array<std::pair<const char*, const char*>, 5> kSectors{{
      {"Industrials", "Specialty Industrial Machinery"},
      {"Technology", "Software - Application"},
      {"Consumer Cyclical", "Specialty Retail"},
      {"Healthcare", "Medical Devices"},
      {"Energy", "Oil & Gas Equipment & Services"},
  }};
  const auto add = [this](const char* prefix, int count, Kind kind) {
    for (int i = 1; i <= count; ++i) {
      Company c;
      c.id = std::string{prefix} + (i < 10 ? "0" : "") + std::to_string(i);
      c.kind = kind;
      switch (kind) {
        case Kind::kIndustrial: {
          const auto& [sector, industry] =
              kSectors.at(static_cast<std::size_t>(rng_.integer(0, 4)));
          c.sector = sector;
          c.industry = industry;
          c.first_year = rng_.chance(0.6) ? rng_.integer(2001, 2004) : rng_.integer(2005, 2014);
          c.revenue = rng_.log_uniform(2e7, 5e9);
          c.fye_month = fiscal_year_end(rng_.uniform());
          c.saturday = rng_.chance(0.12);
          c.nci = rng_.chance(0.15);
          c.temporary_equity = rng_.chance(0.08);
          c.restricted_cash = rng_.chance(0.15);
          c.writedown_ready = rng_.chance(0.3);
          c.exchange = rng_.chance(0.5) ? "XNYS" : "XNAS";
          break;
        }
        case Kind::kBank:
          c.sector = "Financial Services";
          c.industry = "Banks - Regional";
          c.first_year = rng_.integer(2001, 2006);
          c.revenue = rng_.log_uniform(5e7, 2e9);
          c.restricted_cash = rng_.chance(0.25);
          break;
        case Kind::kPreRevenue:
          c.sector = "Healthcare";
          c.industry = "Biotechnology";
          c.first_year = rng_.integer(2008, 2016);
          c.revenue = 0.0;
          c.exchange = "XNAS";
          break;
      }
      c.unit = c.revenue >= 1e9 ? 1e6 : 1e3;
      companies_.push_back(std::move(c));
    }
  };
  add("IND", 40, Kind::kIndustrial);
  add("BNK", 8, Kind::kBank);
  add("PRE", 6, Kind::kPreRevenue);

  // Two foreign filers, reporting in euros.
  for (int done = 0; done < 2;) {
    Company& c = any(Kind::kIndustrial);
    if (c.currency == "USD" && c.fye_month == 12) {
      c.currency = "EUR";
      ++done;
    }
  }
  // Three changes of fiscal year, from December to June or September.
  for (int done = 0; done < 3;) {
    Company& c = any(Kind::kIndustrial);
    if (c.fye_month == 12 && !c.saturday && c.change_year == 0 && c.currency == "USD" &&
        c.first_year <= 2006) {
      c.change_year = rng_.integer(2009, 2018);
      c.new_month = rng_.chance(0.5) ? 6 : 9;
      ++done;
    }
  }
  // Delistings: four industrials and a pre-revenue company stop filing.
  for (int done = 0; done < 5;) {
    Company& c = done < 4 ? any(Kind::kIndustrial) : any(Kind::kPreRevenue);
    if (!c.delisted && c.change_year == 0) {
      c.delisted = true;
      c.last_year = std::min(2024, c.first_year + rng_.integer(7, 12));
      ++done;
    }
  }
  for (Company& c : companies_) {
    plan_quarters(c);
    c.busy.assign(c.quarters.size(), false);
    c.events.assign(c.quarters.size(), Event::kNone);
    if (c.change_year != 0) {
      label_fiscal_year_change(c);
    }
  }
}

// A change of fiscal year: the transition, and a year either side, are the hard
// negative's.
void Universe::label_fiscal_year_change(Company& c) {
  std::size_t t = 0;
  while (c.quarters[t].fiscal_year <= c.change_year) {
    ++t;
  }
  reserve(c, t - 4, std::min(t + 4, c.quarters.size() - 1));
  const std::size_t last_old = t - 1;
  std::size_t first_new_end = t;
  while (!c.quarters[first_new_end].year_end ||
         c.quarters[first_new_end].fiscal_year <= c.change_year + 1) {
    ++first_new_end;
  }
  labels_.emplace_back(c.id, c.quarters[last_old].end, c.quarters[first_new_end].end,
                       "hn_fiscal_year_change", "", "DQ9");
}

void Universe::plan_events(Company& c) {
  if (c.kind != Kind::kIndustrial) {
    return;
  }
  const auto n = static_cast<int>(c.quarters.size());
  if (n < 24) {
    return;
  }
  if (rng_.chance(0.2)) {
    const auto t = static_cast<std::size_t>(rng_.integer(8, n - 10));
    const std::size_t next_year_end =
        year_last(c, std::min(year_last(c, t) + 1, c.quarters.size() - 1));
    if (reserve(c, t, next_year_end)) {
      c.events[t] = Event::kAcquisition;
      labels_.emplace_back(c.id, c.quarters[t].end, c.quarters[next_year_end].end, "hn_acquisition",
                           "", "DQ9");
    }
  }
  if (c.writedown_ready && rng_.chance(0.6)) {
    const auto t = static_cast<std::size_t>(rng_.integer(8, n - 6));
    if (reserve(c, t, t)) {
      c.events[t] = Event::kWritedown;
      labels_.emplace_back(c.id, c.quarters[t].end, c.quarters[year_last(c, t)].end, "hn_writedown",
                           "", "DQ9");
    }
  }
}

void Universe::simulate(Company& c) { Simulation(rng_, c).run(); }

// Splits, where the raw price runs high (or a biotech's low): three at most.
void Universe::plan_splits(Company& c) {
  double f = 1.0;
  std::size_t next = 0;
  for (std::size_t t = 0; t + 3 < c.quarters.size() && c.splits.size() < 3; ++t) {
    if (t < next) {
      continue;
    }
    const Date end = c.quarters[t].end;
    const double raw = mcap_at(c, end) / (c.shares[t] * f);
    Split s;
    if (raw > 200.0 && rng_.chance(0.5)) {
      // 2:1 (60%), 3:1, 3:2 or 4:1.
      static const std::array<std::pair<int, int>, 4> kRatios{{{2, 1}, {3, 1}, {3, 2}, {4, 1}}};
      const double u = rng_.uniform();
      std::size_t k = 0;
      for (const double edge : {0.6, 0.8, 0.9}) {
        k += u >= edge ? 1 : 0;
      }
      s.numerator = kRatios.at(k).first;
      s.denominator = kRatios.at(k).second;
    } else if (c.kind == Kind::kPreRevenue && raw < 1.0 && rng_.chance(0.6)) {
      s.numerator = 1;
      s.denominator = rng_.chance(0.7) ? 10 : 20;
    } else {
      continue;
    }
    s.ex = session_on_or_after(plus_days(end, rng_.integer(20, 75)));
    c.splits.push_back(s);
    f *= s.ratio();
    next = t + 8;
  }
}

void Universe::plan_filings(Company& c) {
  const double revenue = c.values.front()[kRevenue] * 4.0;
  if (revenue >= 1.5e9) {
    c.q_deadline = 40;
    c.k_deadline = 60;
  } else if (revenue >= 1.5e8) {
    c.q_deadline = 40;
    c.k_deadline = 75;
  }
  c.q_lag = c.q_deadline - rng_.integer(3, 12);
  c.k_lag = c.k_deadline - rng_.integer(3, 20);
  c.filed.resize(c.quarters.size());
  c.source.resize(c.quarters.size());
  for (std::size_t t = 0; t < c.quarters.size(); ++t) {
    const Quarter& q = c.quarters[t];
    if (q.fiscal_year < 2004) {
      // SF1 gave no filing date: fafnir estimates the knowledge date.
      c.source[t] = KnowledgeSource::kEstimated;
      c.filed[t] = plus_days(q.end, q.year_end ? 90 : 45);
      continue;
    }
    const int deadline = q.year_end ? c.k_deadline : c.q_deadline;
    const int lag = std::min(deadline, (q.year_end ? c.k_lag : c.q_lag) + rng_.integer(-3, 3));
    Date d = plus_days(q.end, lag);
    while (!calendar_.is_session(d)) {
      d = plus_days(d, -1);
    }
    c.source[t] = KnowledgeSource::kFiled;
    c.filed[t] = d;
  }
  if (c.delisted) {
    c.last_trade = plus_days(c.filed.back(), rng_.integer(20, 90));
  } else {
    c.last_trade = last_session();
  }
}

// A few 10-Qs filed weeks late: one long lag, then the issuer's usual again.
void Universe::late_filings() {
  for (int done = 0, tries = 0; done < 4 && tries < 2000; ++tries) {
    Company& c = any(Kind::kIndustrial);
    const std::size_t t = pick_quarter(c);
    if (t == kNoQuarter || c.quarters[t].year_end || !reserve(c, t, t)) {
      continue;
    }
    c.filed[t] =
        session_on_or_after(plus_days(c.quarters[t].end, c.q_deadline + rng_.integer(15, 40)));
    labels_.emplace_back(c.id, c.quarters[t].end, c.quarters[t].end, "hn_late_filer", "", "DQ9");
    ++done;
  }
}

// When a restatement became known: the next 10-K's filing, which restated its
// comparatives, or a load that found it (the backfill, or a night after). A
// version after the first is always observed, never filed, as fafnir's
// constraints have it (version_no = 1 OR knowledge_source = 'observed').
Date Universe::restatement_date(const Company& c, std::size_t year_end, KnowledgeSource& source,
                                std::optional<Date>& filing) {
  const double u = rng_.uniform();
  std::size_t next = year_end + 1;
  while (next < c.quarters.size() && !c.quarters[next].year_end) {
    ++next;
  }
  source = KnowledgeSource::kObserved;
  filing = std::nullopt;
  if (u < 0.35 && next < c.quarters.size() && c.source[next] == KnowledgeSource::kFiled) {
    return c.filed[next];
  }
  if (u < 0.85) {
    return backfill_day();
  }
  return session_on_or_after(plus_days(backfill_day(), rng_.integer(1, 9)));
}

// Restatements of a fiscal year: its quarters and its year, in one statement. A
// reclassification moves an amount between two components and holds the totals;
// a restatement of the income statement moves values by a few per cent.
void Universe::restatements(Company& c) {
  const int events = c.kind == Kind::kIndustrial ? rng_.integer(0, 3) : rng_.integer(0, 1);
  for (int e = 0, tries = 0; e < events && tries < 20; ++tries) {
    const auto n = static_cast<int>(c.quarters.size());
    if (n < 16) {
      return;
    }
    const auto t = static_cast<std::size_t>(rng_.integer(4, n - 5));
    const std::size_t last = year_last(c, t);
    const std::size_t first = year_first(c, t);
    if (!c.quarters[last].year_end || last - first != 3 || !reserve(c, first, last)) {
      continue;
    }
    ++e;
    const bool bank = c.kind == Kind::kBank;
    // Half of an industrial's restatements are of its income statement, and half
    // of those move values rather than reclassify them.
    Statement s = rng_.chance(0.5) ? kBal : kCfs;
    if (!bank && rng_.chance(0.5)) {
      s = kInc;
    }
    const bool percent = s == kInc && rng_.chance(0.5);
    const double share = rng_.uniform(0.03, 0.15);
    std::array<double, 3> moves{rng_.uniform(-0.04, 0.04), rng_.uniform(-0.04, 0.04),
                                rng_.uniform(-0.04, 0.04)};
    KnowledgeSource source = KnowledgeSource::kFiled;
    std::optional<Date> filing;
    const Date known = restatement_date(c, last, source, filing);
    const double unit = c.unit;
    const auto r = [unit](double v) { return std::round(v / unit) * unit + 0.0; };
    const bool research = c.values[first][kResearch] > 0.0;
    // Current debt to noncurrent, where there is enough of it to move.
    bool debt_move = rng_.chance(0.5);
    for (std::size_t q = first; q <= last; ++q) {
      debt_move = debt_move && c.values[q][kDebtCurrent] >= 100.0 * c.unit;
    }
    std::vector<Row> restated;
    for (std::size_t q = first; q <= last; ++q) {
      Row row = make_row(c, s, PeriodType::kQuarter, q, 2, known, source, filing);
      Values& v = row.values;
      if (percent) {
        const double d_rev = r(v[kRevenue] * moves[0]);
        const double d_cost = r(v[kCostOfRevenue] * moves[1]);
        const double d_sga = r(v[kSga] * moves[2]);
        const double d_op = d_rev - d_cost - d_sga;
        const double d_tax = r(d_op * 0.25);
        const double d_cons = d_op - d_tax;
        const double consolidated = v[kConsolidatedIncome];
        const double d_nci = consolidated != 0.0 ? r(d_cons * v[kNciIncome] / consolidated) : 0.0;
        v[kRevenue] += d_rev;
        v[kCostOfRevenue] += d_cost;
        v[kSga] += d_sga;
        v[kGrossProfit] += d_rev - d_cost;
        v[kOperatingExpenses] += d_sga;
        v[kOperatingIncome] += d_op;
        v[kIncomeBeforeTax] += d_op;
        v[kEbit] += d_op;
        v[kIncomeTax] += d_tax;
        v[kConsolidatedIncome] += d_cons;
        v[kNciIncome] += d_nci;
        v[kNetIncome] += d_cons - d_nci;
        v[kNetIncomeCommon] += d_cons - d_nci;
      } else if (s == kInc) {
        if (research) {  // SG&A to R&D: operating expenses hold
          const double x = r(v[kSga] * share);
          v[kSga] -= x;
          v[kResearch] += x;
        } else {  // cost of revenue to SG&A: operating income holds
          const double x = r(v[kCostOfRevenue] * share);
          v[kCostOfRevenue] -= x;
          v[kSga] += x;
          v[kGrossProfit] += x;
          v[kOperatingExpenses] += x;
        }
      } else if (s == kBal) {
        if (bank) {  // receivables to investments: total assets hold
          const double x = r(v[kReceivables] * share);
          v[kReceivables] -= x;
          v[kInvestments] += x;
        } else if (!debt_move) {  // payables to deferred revenue
          const double x = r(v[kPayables] * share);
          v[kPayables] -= x;
          v[kDeferredRevenue] += x;
        } else {  // current debt to noncurrent: total debt and liabilities hold
          const double x = r(v[kDebtCurrent] * share);
          v[kDebtCurrent] -= x;
          v[kDebtNoncurrent] += x;
          v[kCurrentLiabilities] -= x;
          v[kNoncurrentLiabilities] += x;
        }
      } else {  // operating to investing: the net change holds
        const double x = r(std::abs(v[kOperatingCash]) * share);
        v[kOperatingCash] -= x;
        v[kInvestingCash] += x;
      }
      restated.push_back(row);
    }
    Row annual = make_row(c, s, PeriodType::kAnnual, last, 2, known, source, filing);
    for (std::size_t item = 0; item < kItemCount; ++item) {
      if (info(item).statement != s || !is_money(item)) {
        continue;
      }
      if (info(item).item_class == ItemClass::kFlow) {
        double sum = 0.0;
        for (const Row& row : restated) {
          sum += row.values.at(item);
        }
        annual.values.at(item) = sum;
      } else {
        annual.values.at(item) = restated.back().values.at(item);
      }
    }
    restated.push_back(annual);
    for (const Row& row : restated) {
      c.rows.push_back(row);
    }
    Label label{c.id,
                c.quarters[first].end,
                c.quarters[last].end,
                percent ? "hn_restatement" : "hn_reclassification",
                "",
                "DQ9"};
    label.statement = s;
    labels_.push_back(label);
  }
}

void Universe::label_row(const Company& c, const Row& row, const char* kind, const char* expect,
                         std::string item) {
  Label label{c.id, c.quarters[row.quarter].end, c.quarters[row.quarter].end, kind, expect, ""};
  label.statement = row.statement;
  label.type = row.type;
  label.version = row.version;
  label.item = std::move(item);
  labels_.push_back(std::move(label));
}

// Unit errors: one value, a whole row, and an era of rows, at a power of ten.
void Universe::unit_errors() {
  const auto pick_factor = [this] {
    const double u = rng_.uniform();
    if (u < 0.5) {
      return 1000.0;
    }
    return u < 0.8 ? 0.001 : 1e6;
  };
  for (int done = 0, tries = 0; done < 8 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    if (t == kNoQuarter) {
      continue;
    }
    const Statement s = any_statement();
    const PeriodType type =
        c.quarters[t].year_end && rng_.chance(0.5) ? PeriodType::kAnnual : PeriodType::kQuarter;
    Row* row = find_row(c, s, type, t, 1);
    const auto item = static_cast<std::size_t>(rng_.integer(0, kItemCount - 1));
    if (row == nullptr || info(item).statement != s || !is_money(item) ||
        std::isnan(row->values.at(item)) || std::abs(row->values.at(item)) < 10.0 * c.unit ||
        !reserve(c, t, t)) {
      continue;
    }
    row->values.at(item) *= pick_factor();
    label_row(c, *row, "unit_value", "DQ912", std::string{info(item).column});
    ++done;
  }
  for (int done = 0, tries = 0; done < 5 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    if (t == kNoQuarter || !reserve(c, t, t)) {
      continue;
    }
    const Statement s = any_statement();
    Row& row = row_of(c, s, PeriodType::kQuarter, t, 1);
    const double f = rng_.chance(0.6) ? 1000.0 : 0.001;
    for (std::size_t item = 0; item < kItemCount; ++item) {
      if (is_money(item) && !std::isnan(row.values.at(item))) {
        row.values.at(item) *= f;
      }
    }
    label_row(c, row, "unit_row", "DQ912");
    ++done;
  }
  for (int done = 0, tries = 0; done < 4 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    const auto length = static_cast<std::size_t>(rng_.integer(4, 8));
    if (t == kNoQuarter || t + length + 5 > c.quarters.size() || !reserve(c, t, t + length - 1)) {
      continue;
    }
    const Statement s = any_statement();
    const double f = rng_.chance(0.5) ? 1000.0 : 0.001;
    for (std::size_t q = t; q < t + length; ++q) {
      Row& row = row_of(c, s, PeriodType::kQuarter, q, 1);
      for (std::size_t item = 0; item < kItemCount; ++item) {
        if (is_money(item) && !std::isnan(row.values.at(item))) {
          row.values.at(item) *= f;
        }
      }
    }
    Label label{c.id, c.quarters[t].end, c.quarters[t + length - 1].end, "unit_era", "DQ912", ""};
    label.statement = s;
    label.type = PeriodType::kQuarter;
    label.version = 1;
    labels_.push_back(label);
    ++done;
  }
}

// Sign flips against an item's convention and its own history.
void Universe::sign_flips() {
  static const std::array<Item, 8> kDefinite{kRevenue,         kCostOfRevenue, kSga,
                                             kInterestExpense, kResearch,      kCapex,
                                             kDividendsPaid,   kDepreciation};
  for (int done = 0, tries = 0; done < 6 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    const Item item = kDefinite.at(static_cast<std::size_t>(rng_.integer(0, 7)));
    const Statement s = info(item).statement;
    if (t == kNoQuarter) {
      continue;
    }
    Row& row = row_of(c, s, PeriodType::kQuarter, t, 1);
    if (std::isnan(row.values.at(item)) || std::abs(row.values.at(item)) < 10.0 * c.unit ||
        !reserve(c, t, t)) {
      continue;
    }
    row.values.at(item) = -row.values.at(item);
    label_row(c, row, "sign_flip", "DQ913", std::string{info(item).column});
    ++done;
  }
}

// One value off by a plausible factor, which the next periods revert.
void Universe::bad_values() {
  for (int done = 0, tries = 0; done < 8 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    const auto item = static_cast<std::size_t>(rng_.integer(0, kItemCount - 1));
    if (t == kNoQuarter || !is_money(item)) {
      continue;
    }
    Row& row = row_of(c, info(item).statement, PeriodType::kQuarter, t, 1);
    double& value = row.values.at(item);
    if (std::isnan(value) || std::abs(value) < 100.0 * c.unit || !reserve(c, t, t)) {
      continue;
    }
    const double f = rng_.uniform(1.5, 3.5);
    value = std::round(value * (rng_.chance(0.5) ? f : 1.0 / f) / c.unit) * c.unit;
    label_row(c, row, "bad_value", "DQ911", std::string{info(item).column});
    ++done;
  }
}

// A quarter that repeats the one before it, flows included.
void Universe::copied_periods() {
  for (int done = 0, tries = 0; done < 5 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    if (t == kNoQuarter || !reserve(c, t - 1, t)) {
      continue;
    }
    const Statement s = rng_.chance(0.5) ? kInc : kCfs;
    Row& row = row_of(c, s, PeriodType::kQuarter, t, 1);
    const Row& before = row_of(c, s, PeriodType::kQuarter, t - 1, 1);
    row.values = before.values;
    label_row(c, row, "copied_period", "DQ914");
    ++done;
  }
}

// A zero for an item the company reports in the periods either side.
void Universe::absent_as_zero() {
  static const std::array<Item, 10> kItemsAbsent{
      kSga,      kResearch, kInterestExpense, kCostOfRevenue,    kReceivables, kInventory,
      kPayables, kPpe,      kDepreciation,    kShareCompensation};
  for (int done = 0, tries = 0; done < 6 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    const Item item = kItemsAbsent.at(static_cast<std::size_t>(rng_.integer(0, 9)));
    if (t == kNoQuarter) {
      continue;
    }
    const Statement s = info(item).statement;
    bool reported = true;
    for (std::size_t q = t - 1; q <= t + 1; ++q) {
      const Row& row = row_of(c, s, PeriodType::kQuarter, q, 1);
      reported = reported && !std::isnan(row.values.at(item)) &&
                 std::abs(row.values.at(item)) >= 10.0 * c.unit;
    }
    if (!reported || !reserve(c, t, t)) {
      continue;
    }
    Row& row = row_of(c, s, PeriodType::kQuarter, t, 1);
    row.values.at(item) = 0.0;
    label_row(c, row, "absent_as_zero", "DQ915", std::string{info(item).column});
    ++done;
  }
}

// Quarters stored year to date, and a quarter holding its year's figures.
void Universe::cumulative_quarters() {
  const auto flows = [](Row& row, const auto& value_of) {
    for (std::size_t item = 0; item < kItemCount; ++item) {
      if (info(item).statement == row.statement && info(item).item_class == ItemClass::kFlow) {
        row.values.at(item) = value_of(item);
      }
    }
  };
  for (int done = 0, tries = 0; done < 3 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    if (t == kNoQuarter) {
      continue;
    }
    const std::size_t first = year_first(c, t);
    const std::size_t last = year_last(c, t);
    if (last - first != 3 || !c.quarters[last].year_end || !reserve(c, first, last)) {
      continue;
    }
    const Statement s = rng_.chance(0.5) ? kInc : kCfs;
    std::vector<Values> clean;
    for (std::size_t q = first; q <= last; ++q) {
      clean.push_back(row_of(c, s, PeriodType::kQuarter, q, 1).values);
    }
    for (std::size_t q = first + 1; q <= last; ++q) {
      flows(row_of(c, s, PeriodType::kQuarter, q, 1), [&](std::size_t item) {
        double sum = 0.0;
        for (std::size_t i = 0; i <= q - first; ++i) {
          sum += clean[i].at(item);
        }
        return sum;
      });
    }
    Label label{
        c.id, c.quarters[first + 1].end, c.quarters[last].end, "cumulative_quarters", "DQ916", ""};
    label.statement = s;
    label.type = PeriodType::kQuarter;
    label.version = 1;
    labels_.push_back(label);
    ++done;
  }
  for (int done = 0, tries = 0; done < 3 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    if (t == kNoQuarter) {
      continue;
    }
    const std::size_t last = year_last(c, t);
    if (!c.quarters[last].year_end || last - year_first(c, t) != 3 || !reserve(c, t, t)) {
      continue;
    }
    const Statement s = rng_.chance(0.5) ? kInc : kCfs;
    const Values year = row_of(c, s, PeriodType::kAnnual, last, 1).values;
    Row& row = row_of(c, s, PeriodType::kQuarter, t, 1);
    flows(row, [&](std::size_t item) { return year.at(item); });
    label_row(c, row, "annual_in_quarter", "DQ916");
    ++done;
  }
}

// A total that drops one of its terms, or counts one twice.
void Universe::identity_breaks() {
  for (int done = 0, tries = 0; done < 8 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    const Equation& eq = equations().at(
        static_cast<std::size_t>(rng_.integer(0, static_cast<int>(equations().size()) - 1)));
    if (t == kNoQuarter) {
      continue;
    }
    const PeriodType type =
        c.quarters[t].year_end && rng_.chance(0.3) ? PeriodType::kAnnual : PeriodType::kQuarter;
    Row& row = row_of(c, info(eq.total).statement, type, t, 1);
    const auto& [term, sign] = eq.terms.at(
        static_cast<std::size_t>(rng_.integer(0, static_cast<int>(eq.terms.size()) - 1)));
    const double total = row.values.at(eq.total);
    const double value = row.values.at(term);
    // Well beyond DQ903's tolerance, so that the gap is the term's.
    if (std::isnan(total) || std::isnan(value) ||
        std::abs(value) < std::max(0.05 * std::abs(total), 3.0 * identity_tolerance(row, eq)) ||
        !reserve(c, t, t)) {
      continue;
    }
    const bool doubled = done % 2 == 1;
    row.values.at(eq.total) += (doubled ? 1.0 : -1.0) * sign * value;
    // The gap equals the term: the field DQ903 should name. The total is the value
    // that is wrong, which DQ911 can see where the term is a large part of it.
    const char* kind = doubled ? "identity_doubled_field" : "identity_dropped_field";
    label_row(c, row, kind, "DQ903", std::string{info(term).column});
    label_row(c, row, kind, "DQ911", std::string{info(eq.total).column});
    ++done;
  }
}

// The step a split leaves in the share counts: on file, missing from the actions,
// or a count left in the terms before it.
void Universe::share_faults() {
  int missing = 0;
  int wrong_terms = 0;
  for (Company& c : companies_) {
    for (Split& s : c.splits) {
      // The last quarter known before the split, and the first after it.
      std::size_t after = 0;
      while (after < c.quarters.size() && c.filed[after] < s.ex) {
        ++after;
      }
      if (after == 0 || after >= c.quarters.size()) {
        continue;
      }
      const std::size_t before = after - 1;
      reserve(c, before, after);  // a split's quarters are its own, whatever else is there
      for (std::size_t t = year_first(c, before); t <= year_last(c, after); ++t) {
        c.busy[t] = true;
      }
      if (c.kind == Kind::kIndustrial && missing < 3 && rng_.chance(0.3)) {
        s.on_file = false;
        Label label{c.id, c.quarters[before].end, c.quarters[after].end, "split_missing", "DQ921",
                    ""};
        label.context = true;
        labels_.push_back(label);
        ++missing;
      } else if (wrong_terms < 3 && rng_.chance(0.3)) {
        // The first quarter after the split keeps its counts in the old terms.
        for (Row& row : c.rows) {
          if (row.quarter != after || row.version != 1) {
            continue;
          }
          for (const Item item : {kSharesWeighted, kSharesDiluted, kSharesOutstanding}) {
            if (info(item).statement == row.statement && !std::isnan(row.values.at(item))) {
              row.values.at(item) = std::round(row.values.at(item) / s.ratio());
            }
          }
        }
        Label label{
            c.id, c.quarters[after].end, c.quarters[after].end, "counts_wrong_terms", "DQ921", ""};
        label.version = 1;
        labels_.push_back(label);
        ++wrong_terms;
      } else {
        labels_.emplace_back(c.id, c.quarters[before].end, c.quarters[after].end,
                             "hn_split_on_file", "", "DQ92");
      }
    }
  }
}

// Filing dates: the period end recorded as the filing date, and an amendment's
// date taken for the original's.
void Universe::filing_faults() {
  for (int done = 0, tries = 0; done < 6 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    const bool amendment = done >= 3;
    if (t == kNoQuarter || (amendment && c.quarters[t].year_end) || !reserve(c, t, t)) {
      continue;
    }
    const Date date = amendment
                          ? session_on_or_after(plus_days(c.filed[t + 1], rng_.integer(20, 60)))
                          : c.quarters[t].end;
    for (Row& row : c.rows) {
      if (row.quarter == t && row.version == 1) {
        row.known_from = date;
        row.filing_date = date;
      }
    }
    Label label{c.id,
                c.quarters[t].end,
                c.quarters[t].end,
                amendment ? "amendment_date" : "filing_at_period_end",
                "DQ931",
                ""};
    label.version = 1;
    labels_.push_back(label);
    ++done;
  }
}

// A quarter missing from all three statements.
void Universe::missing_quarters() {
  for (int done = 0, tries = 0; done < 4 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    if (t == kNoQuarter || c.quarters[t].year_end || !reserve(c, t, t)) {
      continue;
    }
    for (Row& row : c.rows) {
      if (row.quarter == t && row.type == PeriodType::kQuarter) {
        row.removed = true;
      }
    }
    Label label{c.id, c.quarters[t].end, c.quarters[t].end, "missing_quarter", "DQ941", ""};
    label.type = PeriodType::kQuarter;
    labels_.push_back(label);
    ++done;
  }
}

// A restatement whose new version is the old at x1,000.
void Universe::restated_at_scale() {
  for (int done = 0, tries = 0; done < 3 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    if (t == kNoQuarter || !reserve(c, t, t)) {
      continue;
    }
    const Statement s = any_statement();
    const PeriodType type =
        c.quarters[t].year_end && rng_.chance(0.5) ? PeriodType::kAnnual : PeriodType::kQuarter;
    KnowledgeSource source = KnowledgeSource::kFiled;
    std::optional<Date> filing;
    const Date known = restatement_date(c, year_last(c, t), source, filing);
    Row row = row_of(c, s, type, t, 1);
    Row restated = make_row(c, s, type, t, 2, known, source, filing);
    for (std::size_t item = 0; item < kItemCount; ++item) {
      if (is_money(item) && !std::isnan(row.values.at(item))) {
        restated.values.at(item) = row.values.at(item) * 1000.0;
      }
    }
    c.rows.push_back(restated);
    // The new version is a whole row at x1000: DQ912's too, in the in-force view.
    label_row(c, restated, "restatement_x1000", "DQ932|DQ912");
    ++done;
  }
}

// DQ901: a row twice, versions that do not chain, and knowledge dates before the
// period's end or apart from the filing date.
void Universe::key_faults() {
  for (int done = 0, tries = 0; done < 3 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    if (t == kNoQuarter || !reserve(c, t, t)) {
      continue;
    }
    const PeriodType type =
        c.quarters[t].year_end && rng_.chance(0.5) ? PeriodType::kAnnual : PeriodType::kQuarter;
    Row& row = row_of(c, any_statement(), type, t, 1);
    row.copies = 2;
    label_row(c, row, "duplicate_row", "DQ901");
    ++done;
  }
  // On restated periods: a gap or an overlap between two versions, two open, and a
  // last version closed.
  static const std::array<const char*, 7> kChains{
      "chain_gap",      "chain_gap",      "chain_overlap",    "chain_overlap",
      "chain_two_open", "chain_two_open", "chain_closed_last"};
  std::vector<std::tuple<std::string, Statement, PeriodType, std::size_t>> used;
  for (std::size_t f = 0, tries = 0; f < kChains.size() && tries < 5000; ++tries) {
    Company& c = any();
    std::vector<Row*> restated;
    for (Row& row : c.rows) {
      if (row.version == 2 && !row.removed) {
        restated.push_back(&row);
      }
    }
    if (restated.empty()) {
      continue;
    }
    Row& later = *restated.at(
        static_cast<std::size_t>(rng_.integer(0, static_cast<int>(restated.size()) - 1)));
    Row* first = find_row(c, later.statement, later.type, later.quarter, 1);
    const auto key = std::tuple{c.id, later.statement, later.type, later.quarter};
    if (first == nullptr || std::find(used.begin(), used.end(), key) != used.end()) {
      continue;
    }
    const std::string_view kind = kChains.at(f);
    const int days = rng_.integer(5, 30);
    if (kind == "chain_gap") {
      if (plus_days(later.known_from, -days) <= first->known_from) {
        continue;
      }
      first->known_to = plus_days(later.known_from, -days);
    } else if (kind == "chain_overlap") {
      first->known_to = plus_days(later.known_from, days);
    } else if (kind == "chain_two_open") {
      first->open = true;
    } else {
      later.known_to = plus_days(later.known_from, days);
    }
    used.push_back(key);
    Label label{c.id,
                c.quarters[later.quarter].end,
                c.quarters[later.quarter].end,
                std::string{kind},
                "DQ901",
                ""};
    label.statement = later.statement;
    label.type = later.type;
    labels_.push_back(std::move(label));
    ++f;
  }
  for (int done = 0, tries = 0; done < 6 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    const bool before = done < 3;
    if (t == kNoQuarter || !reserve(c, t, t)) {
      continue;
    }
    const Date end = c.quarters[t].end;
    // Known before the period ends (and filed then); or known days after the filing.
    const Date known =
        before ? plus_days(end, -rng_.integer(1, 20)) : plus_days(c.filed[t], rng_.integer(3, 20));
    for (Row& row : c.rows) {
      if (row.quarter == t && row.version == 1) {
        row.known_from = known;
        if (before) {
          row.filing_date = known;
        }
      }
    }
    Label label{c.id,    end, end, before ? "known_before_period" : "filing_date_mismatch",
                "DQ901", ""};
    label.version = 1;
    labels_.push_back(std::move(label));
    ++done;
  }
}

// DQ902: a negative where none can be, and a component above its total.
void Universe::impossible_values() {
  static const std::array<Item, 5> kNever{kRevenue, kSharesWeighted, kSharesDiluted, kAssets,
                                          kSharesOutstanding};
  for (int done = 0, tries = 0; done < 4 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    const Item item = kNever.at(static_cast<std::size_t>(rng_.integer(0, 4)));
    if (t == kNoQuarter) {
      continue;
    }
    const PeriodType type =
        c.quarters[t].year_end && rng_.chance(0.3) ? PeriodType::kAnnual : PeriodType::kQuarter;
    Row& row = row_of(c, info(item).statement, type, t, 1);
    double& value = row.values.at(item);
    const double unit = is_money(item) ? c.unit : 1.0;
    if (std::isnan(value) || value < 100.0 * unit || !reserve(c, t, t)) {
      continue;
    }
    // Not a clean sign flip (DQ913's): a small negative, as a sentinel or a slip.
    value = -std::max(unit, std::round(value * rng_.uniform(0.001, 0.2) / unit) * unit);
    // A small negative where the item is large: one bad value (DQ911) as well.
    label_row(c, row, "negative_impossible", "DQ902|DQ911", std::string{info(item).column});
    ++done;
  }
  static const std::array<std::pair<Item, Item>, 4> kParts{{{kCash, kAssets},
                                                            {kCurrentAssets, kAssets},
                                                            {kIntangibles, kAssets},
                                                            {kCurrentLiabilities, kLiabilities}}};
  for (int done = 0, tries = 0; done < 4 && tries < 5000; ++tries) {
    Company& c = any();
    const std::size_t t = pick_quarter(c);
    const auto& [part, total] = kParts.at(static_cast<std::size_t>(rng_.integer(0, 3)));
    if (t == kNoQuarter) {
      continue;
    }
    Row& row = row_of(c, kBal, PeriodType::kQuarter, t, 1);
    const double whole = row.values.at(total);
    if (std::isnan(row.values.at(part)) || std::isnan(whole) || whole <= 0.0 || !reserve(c, t, t)) {
      continue;
    }
    row.values.at(part) = std::round(whole * rng_.uniform(1.05, 1.6) / c.unit) * c.unit;
    label_row(c, row, "component_above_total", "DQ902|DQ911", std::string{info(part).column});
    ++done;
  }
}

Company* Universe::company(const std::string& id) {
  for (Company& c : companies_) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

// The items a row breaks [statements] nonnegative or bounds on, as DQ902 judges
// them: tools/synth/statements.toml's lists.
std::vector<Item> impossible_items(const Row& row) {
  static const std::array<Item, 15> kNonnegative{
      kRevenue,  kCash,        kInvestments,    kReceivables,   kInventory,
      kPpe,      kIntangibles, kCurrentAssets,  kAssets,        kPayables,
      kDeposits, kDebt,        kSharesWeighted, kSharesDiluted, kSharesOutstanding};
  static const std::array<std::pair<Item, Item>, 5> kBounds{{{kCurrentAssets, kAssets},
                                                             {kCash, kAssets},
                                                             {kIntangibles, kAssets},
                                                             {kCurrentLiabilities, kLiabilities},
                                                             {kDebtCurrent, kDebt}}};
  std::vector<Item> broken;
  for (const Item item : kNonnegative) {
    if (row.values.at(item) < 0.0) {  // false for NULL
      broken.push_back(item);
    }
  }
  for (const auto& [lesser, greater] : kBounds) {
    const double a = row.values.at(lesser);
    const double b = row.values.at(greater);
    if (!std::isnan(a) && !std::isnan(b) && a - b > 0.001 * std::abs(b)) {
      broken.push_back(lesser);
    }
  }
  return broken;
}

// A fault that leaves a row impossible is DQ902's too: its label names DQ902 as
// well as its own code, or, where DQ902 reports another item, that item gets a
// label of its own.
void Universe::mark_impossible() {
  const std::size_t count = labels_.size();
  std::vector<std::tuple<std::string, Date, Statement, PeriodType, int, Item>> added;
  for (std::size_t k = 0; k < count; ++k) {
    const Label label = labels_[k];
    Company* c = company(label.series);
    if (label.expect.empty() || c == nullptr) {
      continue;
    }
    for (const Row& row : c->rows) {
      const Date end = c->quarters[row.quarter].end;
      if (row.removed || end < label.first || label.last < end ||
          (label.statement && *label.statement != row.statement) ||
          (label.type && *label.type != row.type) ||
          (label.version && *label.version != row.version)) {
        continue;
      }
      for (const Item item : impossible_items(row)) {
        if (label.item.empty() || label.item == info(item).column) {
          if (labels_[k].expect.find("DQ902") == std::string::npos) {
            labels_[k].expect += "|DQ902";
          }
          continue;
        }
        const auto key = std::tuple{c->id, end, row.statement, row.type, row.version, item};
        if (std::find(added.begin(), added.end(), key) != added.end()) {
          continue;
        }
        added.push_back(key);
        label_row(*c, row, label.kind.c_str(), "DQ902", std::string{info(item).column});
      }
    }
  }
}

// A fault that leaves an identity failing on its row is DQ903's too: the row's gap
// is not the one the company files with (a structural gap) but the fault's.
void Universe::mark_identities() {
  for (Label& label : labels_) {
    Company* c = company(label.series);
    if (label.expect.empty() || label.expect.find("DQ903") != std::string::npos || c == nullptr) {
      continue;
    }
    bool breaks = false;
    for (const Row& row : c->rows) {
      if (!covers(*c, label, row)) {
        continue;
      }
      const Values clean = row.type == PeriodType::kQuarter ? c->values[row.quarter]
                                                            : annual_values(*c, row.quarter);
      for (const Equation& eq : equations()) {
        const std::optional<double> gap = equation_gap(row.values, eq);
        const std::optional<double> was = equation_gap(clean, eq);
        breaks = breaks || (gap && was && identity_breaks_on(row, eq) &&
                            std::abs(*gap - *was) > identity_tolerance(row, eq));
      }
    }
    if (breaks) {
      label.expect += "|DQ903";
    }
  }
}

// The rows in force of a company: each period's highest open version, as dorq's
// in-force view has them with no --as-of.
std::map<std::tuple<Statement, PeriodType, std::size_t>, const Row*> rows_in_force(
    const Company& c) {
  std::map<std::tuple<Statement, PeriodType, std::size_t>, std::vector<const Row*>> versions;
  for (const Row& row : c.rows) {
    if (!row.removed) {
      versions[{row.statement, row.type, row.quarter}].push_back(&row);
    }
  }
  std::map<std::tuple<Statement, PeriodType, std::size_t>, const Row*> in_force;
  for (auto& [key, rows] : versions) {
    std::sort(rows.begin(), rows.end(),
              [](const Row* a, const Row* b) { return a->version < b->version; });
    for (std::size_t k = 0; k < rows.size(); ++k) {
      const bool closed = rows[k]->known_to.has_value() || (k + 1 < rows.size() && !rows[k]->open);
      if (!closed) {
        in_force[key] = rows[k];
      }
    }
  }
  return in_force;
}

// The ties DQ904 judges that fail on a company's rows in force, as the rows of
// each: two balance sheets and a cash flow, or a year's quarters and the year.
std::vector<std::vector<const Row*>> broken_ties(const Company& c) {
  const auto in_force = rows_in_force(c);
  const auto row_at = [&in_force](Statement s, PeriodType type, std::size_t q) -> const Row* {
    const auto it = in_force.find({s, type, q});
    return it == in_force.end() ? nullptr : it->second;
  };
  std::vector<std::vector<const Row*>> broken;
  for (const PeriodType type : {PeriodType::kQuarter, PeriodType::kAnnual}) {
    const auto [low, high] =
        type == PeriodType::kQuarter ? std::pair{56, 126} : std::pair{300, 430};
    const Row* before = nullptr;
    for (std::size_t q = 0; q < c.quarters.size(); ++q) {
      const Row* sheet = row_at(kBal, type, q);
      if (sheet == nullptr) {
        continue;
      }
      const Row* flow = row_at(kCfs, type, q);
      if (before != nullptr && flow != nullptr) {
        const Date end = c.quarters[q].end;
        const int spacing = end.days() - c.quarters[before->quarter].end.days();
        const double a = before->values[kCash];
        const double b = sheet->values[kCash];
        const double gap = (b - a) - flow->values[kNetChangeInCash];
        const double share = end >= restricted_cash_rule() ? 0.10 : 0.02;
        if (spacing >= low && spacing <= high &&
            std::abs(gap) > std::max(share * std::max(std::abs(a), std::abs(b)), 1e6)) {
          broken.push_back({before, sheet, flow});
        }
      }
      before = sheet;
    }
  }
  for (const Statement s : {kInc, kCfs}) {
    std::map<int, std::pair<std::vector<const Row*>, std::vector<const Row*>>> years;
    for (const auto& [key, row] : in_force) {
      if (std::get<0>(key) == s) {
        auto& [quarters, annual] = years[c.quarters[row->quarter].fiscal_year];
        (row->type == PeriodType::kQuarter ? quarters : annual).push_back(row);
      }
    }
    for (const auto& [year, rows] : years) {
      const auto& [quarters, annual] = rows;
      if (quarters.size() != 4 || annual.size() != 1) {
        continue;
      }
      for (std::size_t item = 0; item < kItemCount; ++item) {
        if (info(item).statement != s || info(item).item_class != ItemClass::kFlow) {
          continue;
        }
        double sum = 0.0;
        double scale = std::abs(annual.front()->values.at(item));
        for (const Row* quarter : quarters) {
          sum += quarter->values.at(item);
          scale = std::max(scale, std::abs(quarter->values.at(item)));
        }
        const double gap = sum - annual.front()->values.at(item);
        if (!std::isnan(gap) && std::abs(gap) > std::max(0.005 * scale, 1e6)) {
          std::vector<const Row*> involved = quarters;
          involved.push_back(annual.front());
          broken.push_back(std::move(involved));
          break;
        }
      }
    }
  }
  return broken;
}

// A fault that breaks a tie DQ904 judges is DQ904's too. DQ904 reports the tie on
// one of its rows, which may be another than the fault's (the cash flow after a
// balance sheet era begins, the year of a quarter at the wrong scale): those rows
// get a label of their own, of the fault's kind.
void Universe::mark_ties() {
  std::vector<Label> added;
  std::vector<std::tuple<std::string, Statement, PeriodType, std::size_t, int>> seen;
  for (const Company& c : companies_) {
    for (const std::vector<const Row*>& rows : broken_ties(c)) {
      std::vector<std::size_t> faults;
      for (std::size_t k = 0; k < labels_.size(); ++k) {
        const Label& label = labels_[k];
        if (!label.expect.empty() && std::any_of(rows.begin(), rows.end(), [&](const Row* row) {
              return covers(c, label, *row);
            })) {
          faults.push_back(k);
        }
      }
      if (faults.empty()) {
        continue;
      }
      for (const std::size_t k : faults) {
        if (labels_[k].expect.find("DQ904") == std::string::npos) {
          labels_[k].expect += "|DQ904";
        }
      }
      for (const Row* row : rows) {
        const bool labelled = std::any_of(faults.begin(), faults.end(), [&](std::size_t k) {
          return covers(c, labels_[k], *row);
        });
        const auto key = std::tuple{c.id, row->statement, row->type, row->quarter, row->version};
        if (labelled || std::find(seen.begin(), seen.end(), key) != seen.end()) {
          continue;
        }
        seen.push_back(key);
        Label label{c.id,
                    c.quarters[row->quarter].end,
                    c.quarters[row->quarter].end,
                    labels_[faults.front()].kind,
                    "DQ904",
                    ""};
        label.statement = row->statement;
        label.type = row->type;
        label.version = row->version;
        added.push_back(std::move(label));
      }
    }
  }
  for (Label& label : added) {
    labels_.push_back(std::move(label));
  }
}

void Universe::faults() {
  unit_errors();
  sign_flips();
  bad_values();
  copied_periods();
  absent_as_zero();
  cumulative_quarters();
  identity_breaks();
  filing_faults();
  missing_quarters();
  key_faults();
  impossible_values();
  restated_at_scale();
  mark_impossible();
  mark_identities();
  mark_ties();
}

// Labels that cover a whole series, or a span of one.
void Universe::whole_series_labels() {
  for (const Company& c : companies_) {
    const Date first = c.quarters.front().end;
    const Date last = c.quarters.back().end;
    if (c.kind == Kind::kBank) {
      labels_.emplace_back(c.id, first, last, "hn_bank_nulls", "", "DQ9");
    }
    if (c.kind == Kind::kPreRevenue) {
      labels_.emplace_back(c.id, first, last, "hn_real_zero", "", "DQ91");
    }
    if (c.nci || c.temporary_equity) {
      Label label{c.id, first, last, "hn_structural_gap", "", "DQ90"};
      label.statement = kBal;
      label.identity = "balance_sheet";
      labels_.push_back(label);
    }
    if (c.restricted_cash) {
      std::size_t t = 0;
      while (t < c.quarters.size() && c.quarters[t].end < restricted_cash_rule()) {
        ++t;
      }
      if (t < c.quarters.size()) {
        Label label{c.id, c.quarters[t].end, last, "hn_structural_gap", "", "DQ90"};
        label.identity = "cash_tie";
        labels_.push_back(label);
      }
    }
    if (c.delisted) {
      labels_.emplace_back(c.id, last, newest_end(), "hn_delisted", "", "DQ94");
    }
  }
  labels_.emplace_back("ETF01", Date::from_ymd(2010, 1, 4), newest_end(),
                       "hn_no_statements_expected", "", "DQ94");
  labels_.emplace_back("MIS01", Date::from_ymd(2012, 3, 1), newest_end(), "statements_missing",
                       "DQ943", "");
}

void Universe::build() {
  add_companies();
  for (Company& c : companies_) {
    plan_events(c);
    simulate(c);
    plan_splits(c);
    plan_filings(c);
  }
  late_filings();
  for (Company& c : companies_) {
    first_versions(c);
  }
  share_faults();
  for (Company& c : companies_) {
    restatements(c);
  }
  faults();
  whole_series_labels();
}

// ---------------------------------------------------------------------------
// Writing

std::string loaded_at(Date known) {
  const Date day = std::max(known, backfill_day());
  return day.to_string() + " 06:15:42+00";
}

void Universe::write(const std::filesystem::path& dir) const {
  std::filesystem::create_directories(dir);
  write_statements(dir);
  write_context(dir);
  write_labels(dir);
}

void Universe::write_statements(const std::filesystem::path& dir) const {
  std::ofstream statements(dir / "statements.csv", std::ios::binary);
  statements << "security_id,statement,period_type,fiscal_date,fiscal_year,fiscal_period,"
                "version_no,known_from,known_to,knowledge_source,filing_date,loaded_at,"
                "reported_currency,close";
  for (const ItemInfo& item : kItems) {
    statements << ',' << item.column;
  }
  statements << '\n';
  for (const Company& c : companies_) {
    std::vector<const Row*> rows;
    for (const Row& row : c.rows) {
      if (!row.removed) {
        rows.push_back(&row);
      }
    }
    std::stable_sort(rows.begin(), rows.end(), [&c](const Row* a, const Row* b) {
      return std::tuple{c.quarters[a->quarter].end, a->statement, a->type, a->version} <
             std::tuple{c.quarters[b->quarter].end, b->statement, b->type, b->version};
    });
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const Row& row = *rows[i];
      const Quarter& q = c.quarters[row.quarter];
      // The next version's known_from ends this one's interval, unless a fault
      // says otherwise.
      std::optional<Date> known_to = row.known_to;
      if (!known_to && !row.open && i + 1 < rows.size() && rows[i + 1]->quarter == row.quarter &&
          rows[i + 1]->statement == row.statement && rows[i + 1]->type == row.type) {
        known_to = rows[i + 1]->known_from;
      }
      const bool annual = row.type == PeriodType::kAnnual;
      std::ostringstream line;
      line << c.id << ',' << dorq::to_string(row.statement) << ',' << dorq::to_string(row.type)
           << ',' << q.end.to_string() << ',' << q.fiscal_year << ','
           << (annual ? std::string{"FY"} : "Q" + std::to_string(q.number)) << ',' << row.version
           << ',' << row.known_from.to_string() << ',' << (known_to ? known_to->to_string() : "")
           << ',' << dorq::to_string(row.source) << ','
           << (row.filing_date ? row.filing_date->to_string() : "") << ','
           << loaded_at(row.known_from) << ',' << c.currency << ','
           << price_text(close_at(c, row.known_from));
      for (std::size_t item = 0; item < kItemCount; ++item) {
        line << ',' << integer_text(row.values.at(item));
      }
      line << '\n';
      for (int copy = 0; copy < row.copies; ++copy) {
        statements << line.str();
      }
    }
  }
}

void Universe::write_context(const std::filesystem::path& dir) const {
  std::ofstream items(dir / "items.csv", std::ios::binary);
  items << "statement,column_name,vendor_field,item_class,sign_convention,is_subtotal\n";
  for (const ItemInfo& item : kItems) {
    items << dorq::to_string(item.statement) << ',' << item.column << ',' << item.vendor << ','
          << dorq::to_string(item.item_class) << ',' << dorq::to_string(item.sign) << ','
          << (item.subtotal ? "true" : "false") << '\n';
  }

  std::ofstream actions(dir / "actions.csv", std::ios::binary);
  actions << "security_id,ex_date,action_type,split_numerator,split_denominator,dividend_amount\n";
  for (const Company& c : companies_) {
    for (const Split& s : c.splits) {
      if (s.on_file) {
        actions << c.id << ',' << s.ex.to_string() << ",split," << s.numerator << ','
                << s.denominator << ",\n";
      }
    }
  }

  std::ofstream meta(dir / "meta.csv", std::ios::binary);
  meta << "security_id,asset_type,exchange,currency,first_trade_date,last_trade_date,active,"
          "delisted_date,sector,industry,statements_expected\n";
  for (const Company& c : companies_) {
    meta << c.id << ",equity," << c.exchange << ",USD," << c.first_trade.to_string() << ','
         << c.last_trade.to_string() << ',' << (c.delisted ? "false" : "true") << ','
         << (c.delisted ? plus_days(c.last_trade, 1).to_string() : "") << ',' << c.sector << ','
         << c.industry << ",true\n";
  }
  meta << "ETF01,etf,XNYS,USD,2010-01-04," << last_session().to_string() << ",true,,,,false\n";
  meta << "MIS01,equity,XNAS,USD,2012-03-01," << last_session().to_string()
       << ",true,,Technology,Software - Application,true\n";
}

void Universe::write_labels(const std::filesystem::path& dir) const {
  std::ofstream labels(dir / "labels.csv", std::ios::binary);
  std::ofstream jsonl(dir / "labels.jsonl", std::ios::binary);
  labels << "series,first,last,kind,expect,codes,statement,period_type,version_no,item,identity\n";
  for (const Label& l : labels_) {
    const std::string statement = l.statement ? std::string{dorq::to_string(*l.statement)} : "";
    const std::string type = l.type ? std::string{dorq::to_string(*l.type)} : "";
    const std::string version = l.version ? std::to_string(*l.version) : "";
    labels << l.series << ',' << l.first.to_string() << ',' << l.last.to_string() << ',' << l.kind
           << ',' << l.expect << ',' << l.codes << ',' << statement << ',' << type << ',' << version
           << ',' << l.item << ',' << l.identity << '\n';
    const std::string_view cls = class_of(l);
    jsonl << R"({"series":")" << l.series << R"(","first":")" << l.first.to_string()
          << R"(","last":")" << l.last.to_string() << R"(","class":")" << cls << R"(","kind":")"
          << l.kind << '"';
    if (!l.expect.empty()) {
      jsonl << R"(,"expect":")" << l.expect << '"';
    }
    if (!l.codes.empty()) {
      jsonl << R"(,"codes":")" << l.codes << '"';
    }
    if (!statement.empty()) {
      jsonl << R"(,"statement":")" << statement << '"';
    }
    if (!type.empty()) {
      jsonl << R"(,"period_type":")" << type << '"';
    }
    if (!version.empty()) {
      jsonl << R"(,"version_no":)" << version;
    }
    if (!l.item.empty()) {
      jsonl << R"(,"item":")" << l.item << '"';
    }
    if (!l.identity.empty()) {
      jsonl << R"(,"identity":")" << l.identity << '"';
    }
    jsonl << R"(,"source":"dorq-synth"})" << '\n';
  }
}

// ---------------------------------------------------------------------------
// Verifying

std::vector<std::vector<std::string>> read_csv_file(const std::filesystem::path& path) {
  const std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error(path.string() + ": cannot open the file");
  }
  std::ostringstream text;
  text << in.rdbuf();
  std::vector<std::vector<std::string>> rows;
  dorq::CsvParser parser(',');
  const dorq::CsvParser::RecordFn on_record = [&rows](std::span<const std::string_view> fields,
                                                      std::uint32_t /*line*/) {
    rows.emplace_back(fields.begin(), fields.end());
  };
  parser.feed(text.str(), on_record);
  parser.finish(on_record);
  return rows;
}

struct Cover {
  std::string series;
  Date first;
  Date last;
  bool fault = false;
  std::string kind;
  std::string statement;
  std::string type;
  std::string version;
  std::string identity;
};

std::vector<Cover> read_covers(const std::filesystem::path& path) {
  const auto rows = read_csv_file(path);
  const auto& h = rows.at(0);
  const auto col = [&h](std::string_view name) {
    const auto it = std::find(h.begin(), h.end(), name);
    if (it == h.end()) {
      throw std::runtime_error("labels.csv has no \"" + std::string{name} + "\" column");
    }
    return static_cast<std::size_t>(it - h.begin());
  };
  std::vector<Cover> covers;
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const auto& row = rows[i];
    Cover c;
    c.series = row.at(col("series"));
    c.first = dorq::parse_date(row.at(col("first"))).value_or(Date{});
    c.last = dorq::parse_date(row.at(col("last"))).value_or(Date{});
    c.fault = !row.at(col("expect")).empty();
    c.kind = row.at(col("kind"));
    c.statement = row.at(col("statement"));
    c.type = row.at(col("period_type"));
    c.version = row.at(col("version_no"));
    c.identity = row.at(col("identity"));
    covers.push_back(std::move(c));
  }
  return covers;
}

struct Tally {
  int judged = 0;
  int held = 0;
  int excused = 0;
  int broken = 0;
};

class Verifier {
 public:
  explicit Verifier(std::vector<Cover> covers) : covers_(std::move(covers)) {}

  // Whether a fault label covers the row; or, for `identity`, a structural gap.
  [[nodiscard]] bool covered(const dorq::Series& s, std::size_t i,
                             std::string_view identity = {}) const {
    const dorq::StatementRows& st = s.statements;
    const std::string statement{dorq::to_string(st.statement[i])};
    const std::string type{dorq::to_string(st.period_type[i])};
    const std::string version = std::to_string(st.version[i]);
    return std::any_of(covers_.begin(), covers_.end(), [&](const Cover& c) {
      const bool applies = c.fault || (!identity.empty() && c.identity == identity);
      return applies && c.series == s.id && c.first <= s.date[i] && s.date[i] <= c.last &&
             (c.statement.empty() || c.statement == statement) &&
             (c.type.empty() || c.type == type) && (c.version.empty() || c.version == version);
    });
  }

  // Whether a fault label of the series (and of `statement`, unless that is
  // empty) overlaps [from, to], or a structural gap for `identity` covers `to`.
  [[nodiscard]] bool span_covered(const std::string& series, Date from, Date to,
                                  std::string_view identity, std::string_view statement) const {
    return std::any_of(covers_.begin(), covers_.end(), [&](const Cover& c) {
      if (c.series != series ||
          (!statement.empty() && !c.statement.empty() && c.statement != statement)) {
        return false;
      }
      if (c.fault) {
        return c.first <= to && from <= c.last;
      }
      return c.identity == identity && c.first <= to && to <= c.last;
    });
  }

  void judge(const std::string& rule, bool held, bool excused, const std::string& where) {
    Tally& t = tallies_[rule];
    ++t.judged;
    if (held) {
      ++t.held;
    } else if (excused) {
      ++t.excused;
    } else {
      ++t.broken;
      if (t.broken <= 5) {
        std::cout << "broken: " << rule << " at " << where << "\n";
      }
    }
  }

  [[nodiscard]] const std::map<std::string, Tally>& tallies() const { return tallies_; }

 private:
  std::vector<Cover> covers_;
  std::map<std::string, Tally> tallies_;
};

std::string where(const dorq::Series& s, std::size_t i) {
  const dorq::StatementRows& st = s.statements;
  return s.id + " " + s.date[i].to_string() + " " + std::string{dorq::to_string(st.statement[i])} +
         " " + std::string{dorq::to_string(st.period_type[i])} + " v" +
         std::to_string(st.version[i]);
}

// The rows of one view: for each period, the open version (in force) or the
// earliest (as first reported).
std::vector<std::size_t> view(const dorq::Series& s, bool first_reported) {
  const dorq::StatementRows& st = s.statements;
  std::map<std::tuple<std::int32_t, Statement, PeriodType>, std::size_t> chosen;
  for (std::size_t i = 0; i < st.size(); ++i) {
    const auto key = std::tuple{s.date[i].days(), st.statement[i], st.period_type[i]};
    const auto it = chosen.find(key);
    if (it == chosen.end()) {
      chosen.emplace(key, i);
    } else if (!first_reported && !st.known_to[i]) {
      it->second = i;
    }
  }
  std::vector<std::size_t> rows;
  rows.reserve(chosen.size());
  for (const auto& [key, i] : chosen) {
    rows.push_back(i);
  }
  return rows;
}

void verify_ties(const dorq::Series& s, const dorq::Context& context, bool first_reported,
                 Verifier& v) {
  const dorq::StatementRows& st = s.statements;
  const std::vector<std::size_t> rows = view(s, first_reported);
  const std::string suffix = first_reported ? " (first reported)" : " (in force)";
  // The quarters of a fiscal year sum to the year, for flows.
  for (const std::size_t a : rows) {
    if (st.period_type[a] != PeriodType::kAnnual || st.statement[a] == kBal) {
      continue;
    }
    std::vector<std::size_t> quarters;
    for (const std::size_t q : rows) {
      if (st.period_type[q] == PeriodType::kQuarter && st.statement[q] == st.statement[a] &&
          st.fiscal_year[q] == st.fiscal_year[a] && s.date[q] <= s.date[a]) {
        quarters.push_back(q);
      }
    }
    bool held = !quarters.empty();
    for (const dorq::LineItem& item : context.items) {
      const std::vector<double>* values = st.item(item.column);
      if (item.statement != st.statement[a] || item.item_class != ItemClass::kFlow ||
          values == nullptr || std::isnan((*values)[a])) {
        continue;
      }
      double sum = 0.0;
      for (const std::size_t q : quarters) {
        sum += (*values)[q];
      }
      held = held && std::abs(sum - (*values)[a]) <= 0.5;
    }
    // The fiscal year's quarter ends, a missing one's included: 273 days before the
    // year end for Q1, and 364 for the year before's Q4.
    const Date from = plus_days(s.date[a], -300);
    v.judge("quarter_sum" + suffix, held,
            v.span_covered(s.id, from, s.date[a], "quarter_sum", dorq::to_string(st.statement[a])),
            where(s, a));
  }
  // The change in cash is the cash flow's net change, quarter to quarter.
  const std::vector<double>* cash = st.item("cash_and_equivalents");
  const std::vector<double>* change = st.item("net_change_in_cash");
  if (cash == nullptr || change == nullptr) {
    return;
  }
  bool have_previous = false;
  std::size_t previous = 0;
  for (const std::size_t b : rows) {
    if (st.period_type[b] != PeriodType::kQuarter || st.statement[b] != kBal) {
      continue;
    }
    if (have_previous) {
      const auto flow = std::find_if(rows.begin(), rows.end(), [&](std::size_t f) {
        return st.period_type[f] == PeriodType::kQuarter && st.statement[f] == kCfs &&
               s.date[f] == s.date[b];
      });
      if (flow != rows.end()) {
        const bool held = std::abs(((*cash)[b] - (*cash)[previous]) - (*change)[*flow]) <= 0.5;
        v.judge("cash_tie" + suffix, held,
                v.span_covered(s.id, s.date[previous], s.date[b], "cash_tie", {}), where(s, b));
      }
    }
    have_previous = true;
    previous = b;
  }
}

// Holds the identities, non-negative items and bounds on each row of a series.
void verify_rows(const dorq::Series& s, const dorq::StatementSettings& settings, Verifier& v) {
  const dorq::StatementRows& st = s.statements;
  const auto value = [&st](const std::string& item, std::size_t i) {
    const std::vector<double>* column = st.item(item);
    return column == nullptr ? kNull : (*column)[i];
  };
  for (std::size_t i = 0; i < st.size(); ++i) {
    for (const dorq::Identity& identity : settings.identities) {
      double gap = 0.0;
      bool complete = true;
      const std::array<std::pair<const std::vector<dorq::IdentityTerm>*, double>, 2> sides{
          {{&identity.left, 1.0}, {&identity.right, -1.0}}};
      for (const auto& [terms, sign] : sides) {
        for (const dorq::IdentityTerm& term : *terms) {
          const double x = value(term.item, i);
          complete = complete && !std::isnan(x);
          gap += sign * (term.negative ? -x : x);
        }
      }
      if (complete) {
        v.judge(identity.name, std::abs(gap) <= 0.5, v.covered(s, i, identity.name), where(s, i));
      }
    }
    for (const std::string& item : settings.nonnegative) {
      const double x = value(item, i);
      if (!std::isnan(x)) {
        v.judge("nonnegative", x >= 0.0, v.covered(s, i), where(s, i) + " " + item);
      }
    }
    for (const dorq::ItemBound& bound : settings.bounds) {
      const double lesser = value(bound.lesser, i);
      const double greater = value(bound.greater, i);
      if (!std::isnan(lesser) && !std::isnan(greater)) {
        v.judge(bound.to_string(), lesser <= greater, v.covered(s, i), where(s, i));
      }
    }
  }
}

// Prints a line per rule. Returns the number of breaks, or -1 when a rule holds on
// no row.
int report(const Verifier& v, const dorq::StatementSettings& settings) {
  std::cout
      << "rule                                              judged    held  excused  broken\n";
  int broken = 0;
  bool vacuous = false;
  for (const auto& [rule, t] : v.tallies()) {
    std::string line = rule;
    const auto cell = [&line](int n, std::size_t width) {
      const std::string text = std::to_string(n);
      line += std::string(width > text.size() ? width - text.size() : 1, ' ') + text;
    };
    line.resize(std::max<std::size_t>(line.size(), 48), ' ');
    cell(t.judged, 8);
    cell(t.held, 8);
    cell(t.excused, 9);
    cell(t.broken, 8);
    std::cout << line << "\n";
    broken += t.broken;
    vacuous = vacuous || t.held == 0;
  }
  for (const dorq::Identity& identity : settings.identities) {
    if (!v.tallies().contains(identity.name)) {
      std::cout << "the identity " << identity.name << " judges no row\n";
      vacuous = true;
    }
  }
  return vacuous ? -1 : broken;
}

}  // namespace

void generate_statements(std::uint64_t seed, const std::filesystem::path& dir) {
  Universe universe(seed);
  universe.build();
  universe.write(dir);
}

int verify_statements(const std::filesystem::path& dir, const std::filesystem::path& config_path) {
  const dorq::Config config = dorq::load_config(config_path);
  dorq::Context context;
  {
    std::ifstream in(dir / "items.csv", std::ios::binary);
    dorq::read_items(in, "items.csv", context);
  }
  for (const dorq::ContextIssue& issue : context.issues) {
    std::cout << "DQ109: " << issue.source << " line " << issue.line << ": " << issue.message
              << "\n";
  }
  std::vector<dorq::Series> all;
  dorq::ReadOptions options;
  options.kind = dorq::KindOption::kStatements;
  options.items = context.item_names();
  dorq::SeriesAssembler assembler(dorq::Grouping::kBuffer,
                                  [&all](dorq::Series&& s) { all.push_back(std::move(s)); });
  {
    std::ifstream in(dir / "statements.csv", std::ios::binary);
    dorq::read_input(in, "statements.csv", "statements", ".csv", options, assembler);
  }
  assembler.finish();
  std::size_t issues = 0;
  for (dorq::Series& s : all) {
    dorq::sort_by_date(s);
    for (const dorq::FieldIssue& issue : s.issues) {
      if (++issues <= 5) {
        std::cout << "DQ104: " << s.id << " line " << issue.line << " " << issue.column << " \""
                  << issue.text << "\"\n";
      }
    }
  }
  Verifier v(read_covers(dir / "labels.csv"));
  for (const dorq::Series& s : all) {
    verify_rows(s, config.statements, v);
    verify_ties(s, context, false, v);
    verify_ties(s, context, true, v);
  }
  const int broken = report(v, config.statements);
  if (issues > 0 || !context.issues.empty()) {
    std::cout << issues + context.issues.size() << " rows dorq cannot use\n";
    return -1;
  }
  if (broken < 0) {
    return -1;
  }
  std::cout << (broken == 0 ? "every rule holds on the rows with no fault\n"
                            : std::to_string(broken) + " breaks\n");
  return broken;
}

}  // namespace synth
