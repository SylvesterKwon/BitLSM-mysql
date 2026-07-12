# The battery of differential queries for bitlsm_index_read, sourced once with
# the data in the memtable and once after a forced flush (data in an SST), so
# both storage states are exercised. Each query is checked with
# bitlsm_index_read_check.inc (FORCE INDEX(bi) must equal the full scan).

# (a) multiple AND conditions, mixing >=, < and a VARBINARY equality.
--let $qtitle = Q1 multi-AND (a>=0 AND b<100 AND c='gamma')
--let $w = a >= 0 AND b < 100 AND c = 'gamma'
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# (b) empty result.
--let $qtitle = Q2 empty result (a > 1000)
--let $w = a > 1000
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# (c) all rows match (index-column tautology that also admits NULL a).
--let $qtitle = Q3 all rows (a >= -2147483648 OR a IS NULL)
--let $w = a >= -2147483648 OR a IS NULL
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# (e) equality on a signed INT column.
--let $qtitle = Q4 INT equality (a = 10)
--let $w = a = 10
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# (d) NULL exclusion: a comparison predicate must drop the NULL-a row.
--let $qtitle = Q5 boundary >= drops NULL a (a >= 10)
--let $w = a >= 10
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# (d) explicit IS NULL on an index column.
--let $qtitle = Q6 IS NULL on b (b IS NULL)
--let $w = b IS NULL
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# (e) DOUBLE range, both bounds.
--let $qtitle = Q7 DOUBLE range (b >= 0.0 AND b < 50.0)
--let $w = b >= 0.0 AND b < 50.0
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# (f) VARBINARY equality selecting a single row.
--let $qtitle = Q8 VARBINARY equality (c = 'x')
--let $w = c = 'x'
--source suite/rocksdb/include/bitlsm_index_read_check.inc
