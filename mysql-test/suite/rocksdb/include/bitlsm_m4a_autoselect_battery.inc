# M4a auto-selection battery. Sourced once with data in the memtable and once
# after a forced flush. Each block:
#   1. EXPLAIN (no FORCE INDEX) and machine-assert key = bi.
#   2. Differential: the auto-selected result equals IGNORE INDEX (full scan).

--echo #
--echo # (1) THE ONE THING: no 'a' predicate, no hint -> optimizer must pick bi.
--replace_column 10 # 11 #
EXPLAIN SELECT id, a, b, c, d FROM t_read WHERE b < 100 AND c = 'gamma';
# NB: query_get_value() splits its arguments on commas, so the probe query must
# have a comma-free select list. 'd' is not in bi, keeping the read non-covering.
--let $auto_key = query_get_value(EXPLAIN SELECT d FROM t_read WHERE b < 100 AND c = 'gamma', key, 1)
--let $assert_text = auto-selected (no hint) non-leading predicate routes to bi
--let $assert_cond = "$auto_key" = "bi"
--source include/assert.inc

--let $qtitle = A1 non-leading AND (b<100 AND c='gamma'), auto-selected
--let $w = b < 100 AND c = 'gamma'
--source suite/rocksdb/include/bitlsm_m4a_autoselect_check.inc

--echo #
--echo # (2) single non-leading equality (c only), no hint.
--let $auto_key = query_get_value(EXPLAIN SELECT d FROM t_read WHERE c = 'gamma', key, 1)
--let $assert_text = auto-selected (no hint) single non-leading equality routes to bi
--let $assert_cond = "$auto_key" = "bi"
--source include/assert.inc

--let $qtitle = A2 single non-leading equality (c='gamma'), auto-selected
--let $w = c = 'gamma'
--source suite/rocksdb/include/bitlsm_m4a_autoselect_check.inc

--echo #
--echo # (3) middle-column range only (b), no hint.
--let $qtitle = A3 middle-column range (b >= 0.0 AND b < 50.0), auto-selected
--let $w = b >= 0.0 AND b < 50.0
--source suite/rocksdb/include/bitlsm_m4a_autoselect_check.inc

--echo #
--echo # (4) REGRESSION: a LEADING 'a' predicate must still work (normal path).
--let $lead_key = query_get_value(EXPLAIN SELECT d FROM t_read WHERE a >= 10, key, 1)
--let $assert_text = leading predicate (a>=10) still routes to bi
--let $assert_cond = "$lead_key" = "bi"
--source include/assert.inc

--let $qtitle = A4 leading predicate (a>=10), auto-selected
--let $w = a >= 10
--source suite/rocksdb/include/bitlsm_m4a_autoselect_check.inc

--echo #
--echo # (5) empty result via non-leading predicate.
--let $qtitle = A5 non-leading empty (c='nope')
--let $w = c = 'nope'
--source suite/rocksdb/include/bitlsm_m4a_autoselect_check.inc
