# DATE differential battery, sourced once with the data in the memtable and
# once after a forced flush (SABI block present), so both storage states are
# exercised. Every case is checked with bitlsm_date_read_check.inc.

# Leap-day equality: the packing must be exact, not merely monotone.
--let $qtitle = D1 leap-day equality (d = '2024-02-29')
--let $w = d = '2024-02-29'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# Year is the high field of the packing, so an endianness or shift error shows
# up on a year-boundary range first.
--let $qtitle = D2 year-boundary range (d >= '1999-12-31' AND d < '2000-01-02')
--let $w = d >= '1999-12-31' AND d < '2000-01-02'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

--let $qtitle = D3 BETWEEN over a month end (d BETWEEN '2020-01-31' AND '2020-02-01')
--let $w = d BETWEEN '2020-01-31' AND '2020-02-01'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

--let $qtitle = D4 DATE IN-list (three dates)
--let $w = d IN ('2000-02-29', '2020-06-15', '9999-12-31')
--source suite/rocksdb/include/bitlsm_date_read_check.inc

--let $qtitle = D5 domain bounds (d <= '1000-01-01' OR d >= '9999-12-31')
--let $w = d <= '1000-01-01' OR d >= '9999-12-31'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

--let $qtitle = D6 NULL exclusion (d >= '1000-01-01')
--let $w = d >= '1000-01-01'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# IS NULL is not translatable; results must still be correct.
--let $qtitle = D7 IS NULL on d
--let $w = d IS NULL
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# A comparand with a time part is compared as DATETIME, so d = 2020-06-15
# matches and the translator must omit it rather than truncate. Const folding
# may rewrite a bare literal first, so no string is asserted; D11/D12 are exact.
--let $qtitle = D8 time-part comparand, shape stays correct either way (d < '2020-06-15 10:30:00')
--let $w = d < '2020-06-15 10:30:00'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

--let $qtitle = D9 INT + DATE conjunction (a >= 5 AND d < '2024-01-01')
--let $w = a >= 5 AND d < '2024-01-01'
--source suite/rocksdb/include/bitlsm_date_read_check.inc

--let $qtitle = D10 row IN over (a,d)
--let $w = (a, d) IN ((7, '2020-06-15'), (8, '2024-02-29'))
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# Const folding leaves IN-lists untouched, so the translator sees the time-part
# literal and must omit it; d = 2020-06-15 matches that literal.
--let $qtitle = D11 time-part comparand inside an IN list
--let $w = d IN ('2020-06-15 10:30:00', '2024-02-29')
--source suite/rocksdb/include/bitlsm_date_read_check.inc

# Same guard for a BETWEEN bound, which const folding also leaves untouched.
--let $qtitle = D12 time-part comparand as a BETWEEN bound
--let $w = d BETWEEN '2000-01-01' AND '2020-06-15 10:30:00'
--source suite/rocksdb/include/bitlsm_date_read_check.inc
