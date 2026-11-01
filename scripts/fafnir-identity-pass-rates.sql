-- Each candidate identity's pass rate on fafnir's current versions (DR-0905, plan
-- §4.2), with DQ903's tolerance: the larger of 0.5% of the largest term and 1M of
-- the reporting currency. Aggregates only: post the result on
-- rtrimble13/dorq#19. An identity that holds on 100% of rows holds by
-- construction (SF1 computes it) and is dropped from priors/fafnir-statements.toml;
-- one below 99% is investigated before it ships at warn.
--
--   psql "$FAFNIR_DB" -f scripts/fafnir-identity-pass-rates.sql
WITH rates AS (
SELECT 'balance_sheet' AS identity, 'core.balance_sheet' AS table_name, count(*) AS judged,
       count(*) FILTER (WHERE abs(total_assets - total_liabilities - total_equity_parent)
                        <= greatest(0.005 * greatest(abs(total_assets), abs(total_liabilities), abs(total_equity_parent)), 1000000)) AS held
  FROM core.balance_sheet
 WHERE superseded_at IS NULL AND total_assets IS NOT NULL AND total_liabilities IS NOT NULL AND total_equity_parent IS NOT NULL
UNION ALL
SELECT 'assets_split' AS identity, 'core.balance_sheet' AS table_name, count(*) AS judged,
       count(*) FILTER (WHERE abs(total_assets - total_current_assets - total_noncurrent_assets)
                        <= greatest(0.005 * greatest(abs(total_assets), abs(total_current_assets), abs(total_noncurrent_assets)), 1000000)) AS held
  FROM core.balance_sheet
 WHERE superseded_at IS NULL AND total_assets IS NOT NULL AND total_current_assets IS NOT NULL AND total_noncurrent_assets IS NOT NULL
UNION ALL
SELECT 'liabilities_split' AS identity, 'core.balance_sheet' AS table_name, count(*) AS judged,
       count(*) FILTER (WHERE abs(total_liabilities - total_current_liabilities - total_noncurrent_liabilities)
                        <= greatest(0.005 * greatest(abs(total_liabilities), abs(total_current_liabilities), abs(total_noncurrent_liabilities)), 1000000)) AS held
  FROM core.balance_sheet
 WHERE superseded_at IS NULL AND total_liabilities IS NOT NULL AND total_current_liabilities IS NOT NULL AND total_noncurrent_liabilities IS NOT NULL
UNION ALL
SELECT 'debt_split' AS identity, 'core.balance_sheet' AS table_name, count(*) AS judged,
       count(*) FILTER (WHERE abs(total_debt - debt_current - debt_noncurrent)
                        <= greatest(0.005 * greatest(abs(total_debt), abs(debt_current), abs(debt_noncurrent)), 1000000)) AS held
  FROM core.balance_sheet
 WHERE superseded_at IS NULL AND total_debt IS NOT NULL AND debt_current IS NOT NULL AND debt_noncurrent IS NOT NULL
UNION ALL
SELECT 'investments_split' AS identity, 'core.balance_sheet' AS table_name, count(*) AS judged,
       count(*) FILTER (WHERE abs(investments_total - investments_current - investments_noncurrent)
                        <= greatest(0.005 * greatest(abs(investments_total), abs(investments_current), abs(investments_noncurrent)), 1000000)) AS held
  FROM core.balance_sheet
 WHERE superseded_at IS NULL AND investments_total IS NOT NULL AND investments_current IS NOT NULL AND investments_noncurrent IS NOT NULL
UNION ALL
SELECT 'gross_profit' AS identity, 'core.income_statement' AS table_name, count(*) AS judged,
       count(*) FILTER (WHERE abs(gross_profit - revenue + cost_of_revenue)
                        <= greatest(0.005 * greatest(abs(gross_profit), abs(revenue), abs(cost_of_revenue)), 1000000)) AS held
  FROM core.income_statement
 WHERE superseded_at IS NULL AND gross_profit IS NOT NULL AND revenue IS NOT NULL AND cost_of_revenue IS NOT NULL
UNION ALL
SELECT 'operating_income' AS identity, 'core.income_statement' AS table_name, count(*) AS judged,
       count(*) FILTER (WHERE abs(operating_income - gross_profit + operating_expenses)
                        <= greatest(0.005 * greatest(abs(operating_income), abs(gross_profit), abs(operating_expenses)), 1000000)) AS held
  FROM core.income_statement
 WHERE superseded_at IS NULL AND operating_income IS NOT NULL AND gross_profit IS NOT NULL AND operating_expenses IS NOT NULL
UNION ALL
SELECT 'ebit' AS identity, 'core.income_statement' AS table_name, count(*) AS judged,
       count(*) FILTER (WHERE abs(ebit - income_before_tax - interest_expense)
                        <= greatest(0.005 * greatest(abs(ebit), abs(income_before_tax), abs(interest_expense)), 1000000)) AS held
  FROM core.income_statement
 WHERE superseded_at IS NULL AND ebit IS NOT NULL AND income_before_tax IS NOT NULL AND interest_expense IS NOT NULL
UNION ALL
SELECT 'net_income' AS identity, 'core.income_statement' AS table_name, count(*) AS judged,
       count(*) FILTER (WHERE abs(net_income - consolidated_income + net_income_noncontrolling)
                        <= greatest(0.005 * greatest(abs(net_income), abs(consolidated_income), abs(net_income_noncontrolling)), 1000000)) AS held
  FROM core.income_statement
 WHERE superseded_at IS NULL AND net_income IS NOT NULL AND consolidated_income IS NOT NULL AND net_income_noncontrolling IS NOT NULL
UNION ALL
SELECT 'net_income_common' AS identity, 'core.income_statement' AS table_name, count(*) AS judged,
       count(*) FILTER (WHERE abs(net_income_common - net_income + preferred_dividends)
                        <= greatest(0.005 * greatest(abs(net_income_common), abs(net_income), abs(preferred_dividends)), 1000000)) AS held
  FROM core.income_statement
 WHERE superseded_at IS NULL AND net_income_common IS NOT NULL AND net_income IS NOT NULL AND preferred_dividends IS NOT NULL
UNION ALL
SELECT 'cash_flow' AS identity, 'core.cash_flow_statement' AS table_name, count(*) AS judged,
       count(*) FILTER (WHERE abs(net_change_in_cash - net_cash_operating - net_cash_investing - net_cash_financing - fx_effect_on_cash)
                        <= greatest(0.005 * greatest(abs(net_change_in_cash), abs(net_cash_operating), abs(net_cash_investing), abs(net_cash_financing), abs(fx_effect_on_cash)), 1000000)) AS held
  FROM core.cash_flow_statement
 WHERE superseded_at IS NULL AND net_change_in_cash IS NOT NULL AND net_cash_operating IS NOT NULL AND net_cash_investing IS NOT NULL AND net_cash_financing IS NOT NULL AND fx_effect_on_cash IS NOT NULL
)
SELECT identity, table_name, judged, held, judged - held AS failed,
       round(100.0 * held / nullif(judged, 0), 3) AS pass_rate_pct
  FROM rates
 ORDER BY table_name, identity;
