# DATE differential battery, sourced once with the data in the memtable and
# once after a forced flush (SABI block present), so both storage states are
# exercised. Every case is checked with bitlsm_date_read_check.inc.

# (a) equality on a leap day -- the packing must be exact, not merely monotone.
--let $qtitle = D1 leap-day equality (d = '2024-02-29')
--let $w = d = '2024-02-29'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# (b) range crossing a year boundary: year is the high field of the packing,
# so an endianness or shift error shows up here first.
--let $qtitle = D2 year-boundary range (d >= '1999-12-31' AND d < '2000-01-02')
--let $w = d >= '1999-12-31' AND d < '2000-01-02'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# (c) BETWEEN spanning a month end -> two clause_groups (>= lo, <= hi).
--let $qtitle = D3 BETWEEN over a month end (d BETWEEN '2020-01-31' AND '2020-02-01')
--let $w = d BETWEEN '2020-01-31' AND '2020-02-01'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# (d) IN-list -> one OR-clause of DATE equalities.
--let $qtitle = D4 DATE IN-list (three dates)
--let $w = d IN ('2000-02-29', '2020-06-15', '9999-12-31')
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# (e) the supported min/max dates, at both okey extremes.
--let $qtitle = D5 domain bounds (d <= '1000-01-01' OR d >= '9999-12-31')
--let $w = d <= '1000-01-01' OR d >= '9999-12-31'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# (f) a comparison predicate must drop the NULL-d row.
--let $qtitle = D6 NULL exclusion (d >= '1000-01-01')
--let $w = d >= '1000-01-01'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# (g) explicit IS NULL (unrepresentable -> omitted, results still correct).
--let $qtitle = D7 IS NULL on d
--let $w = d IS NULL
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# (h) SHAPE STAYS CORRECT EITHER WAY. A comparand carrying a time part is
# compared as DATETIME: the DATE column widens to midnight, so d = 2020-06-15
# MATCHES here. Truncating to the date part and emitting
# `okey < packed(2020-06-15)` would drop that row -- a STRENGTHENING. But for
# THIS shape (a bare comparison), constant folding
# (sql/sql_const_folding.cc::analyze_timestamp_field_constant) truncates the
# time part for a DATE field and rewrites `<` to `<=` before the translator
# ever sees the Item tree, so in practice the translator likely receives a
# clean DATE literal here and the guard (non_zero_time()) never fires. This
# case is deliberately NOT asserted as a golden string: whether MySQL folds the
# constant down to a DATE first is a server-side detail, and either way the row
# set must be identical to the full scan. D11/D12 below exercise shapes const
# folding does not touch, so they actually reach the guard.
--let $qtitle = D8 time-part comparand, shape stays correct either way (d < '2020-06-15 10:30:00')
--let $w = d < '2020-06-15 10:30:00'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# (i) DATE combined with the INT attribute -> two clause_groups on two attrs.
--let $qtitle = D9 INT + DATE conjunction (a >= 5 AND d < '2024-01-01')
--let $w = a >= 5 AND d < '2024-01-01'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# (j) row-constructor IN over (a, d) -- exercises both features at once. Before
# the row-IN task lands this prunes nothing and still passes; after it lands it
# must STILL pass, now with two distributed clause_groups.
--let $qtitle = D10 row IN over (a,d)
--let $w = (a, d) IN ((7, '2020-06-15'), (8, '2024-02-29'))
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# (k) THE STRENGTHENING GUARD, for real this time. Const folding
# (sql/sql_const_folding.cc) only normalizes EQ/NE/LT/LE/GE/GT/EQUAL/
# MULT_EQUAL -- an IN-list falls to its default case and is left untouched, so
# the translator sees the raw '2020-06-15 10:30:00' literal, with its time
# part intact, and non_zero_time() actually fires here. d = 2020-06-15
# MATCHES this literal (DATE widens to midnight), so omitting the comparand
# is required to avoid losing that row.
--let $qtitle = D11 time-part comparand inside an IN list
--let $w = d IN ('2020-06-15 10:30:00', '2024-02-29')
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# (l) Same guard, BETWEEN shape: BETWEEN is likewise not touched by const
# folding (it only rewrites plain binary comparisons), so the upper bound
# reaches the translator with its time part intact and non_zero_time() fires
# on it, omitting that clause_group rather than emitting a strengthened
# `okey <= packed(2020-06-15)` that would drop the 2020-06-15 row.
--let $qtitle = D12 time-part comparand as a BETWEEN bound
--let $w = d BETWEEN '2000-01-01' AND '2020-06-15 10:30:00'
--source suite/rocksdb/include/bitlsm_date_read_check.inc
