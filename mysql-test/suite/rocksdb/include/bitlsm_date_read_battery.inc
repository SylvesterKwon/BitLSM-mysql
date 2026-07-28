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

# (h) THE STRENGTHENING GUARD. A comparand carrying a time part is compared as
# DATETIME: the DATE column widens to midnight, so d = 2020-06-15 MATCHES here.
# Truncating to the date part and emitting `okey < packed(2020-06-15)` would
# drop that row. The translator must omit the condition instead. This case is
# deliberately NOT asserted as a golden string: whether MySQL folds the constant
# down to a DATE first is a server-side detail, and either way the row set must
# be identical to the full scan.
--let $qtitle = D8 time-part comparand must not lose rows (d < '2020-06-15 10:30:00')
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
