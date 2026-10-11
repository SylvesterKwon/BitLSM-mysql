# The battery of differential queries for bitlsm_index_read, sourced once with
# the data in the memtable and once after a forced flush (data in an SST), so
# both storage states are exercised. Each query is checked with
# bitlsm_index_read_check.inc (FORCE INDEX(bi) must equal the full scan).

--let $qtitle = Q1 multi-AND (a>=0 AND b<100 AND c='gamma')
--let $w = a >= 0 AND b < 100 AND c = 'gamma'
--source suite/rocksdb/include/bitlsm_index_read_check.inc

--let $qtitle = Q2 empty result (a > 1000)
--let $w = a > 1000
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# Index-column tautology that also admits NULL a.
--let $qtitle = Q3 all rows (a >= -2147483648 OR a IS NULL)
--let $w = a >= -2147483648 OR a IS NULL
--source suite/rocksdb/include/bitlsm_index_read_check.inc

--let $qtitle = Q4 INT equality (a = 10)
--let $w = a = 10
--source suite/rocksdb/include/bitlsm_index_read_check.inc

--let $qtitle = Q5 boundary >= drops NULL a (a >= 10)
--let $w = a >= 10
--source suite/rocksdb/include/bitlsm_index_read_check.inc

--let $qtitle = Q6 IS NULL on b (b IS NULL)
--let $w = b IS NULL
--source suite/rocksdb/include/bitlsm_index_read_check.inc

--let $qtitle = Q7 DOUBLE range (b >= 0.0 AND b < 50.0)
--let $w = b >= 0.0 AND b < 50.0
--source suite/rocksdb/include/bitlsm_index_read_check.inc

--let $qtitle = Q8 VARBINARY equality (c = 'x')
--let $w = c = 'x'
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# Row-constructor IN distributes per column into (a=10 OR a=25) AND (c='gamma'
# OR c='delta'); the extra combinations it admits are dropped by ICP.
--let $qtitle = Q9 row IN over two indexed columns
--let $w = (a, c) IN ((10, 'gamma'), (25, 'delta'))
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# A row constructor mixing an indexed and a non-indexed column is dropped before
# ICP (uses_index_fields_only), so this prunes nothing; it pins the unpruned path.
--let $qtitle = Q10 row IN with a non-indexed column
--let $w = (a, d) IN ((10, 300), (25, 500))
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# Each distributed clause matches rows but no single row satisfies a pair: this
# exposes a distribution treated as exact rather than as a weakening.
--let $qtitle = Q11 row IN with no matching pair
--let $w = (a, c) IN ((10, 'delta'), (25, 'gamma'))
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# A negated row IN is not representable; the translator must emit nothing rather
# than the positive form.
--let $qtitle = Q12 row NOT IN (must translate to nothing, stay correct)
--let $w = (a, c) NOT IN ((10, 'gamma'), (25, 'delta'))
--source suite/rocksdb/include/bitlsm_index_read_check.inc
