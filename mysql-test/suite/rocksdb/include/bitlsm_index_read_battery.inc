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

# (g) row-constructor IN: distributes into one clause_group per column,
# (a=10 OR a=25) AND (c='gamma' OR c='delta'). The extra combinations that
# admits (a=10 AND c='delta') are dropped by the ICP re-verify, so the row set
# must still match the full scan exactly.
--let $qtitle = Q9 row IN over two indexed columns
--let $w = (a, c) IN ((10, 'gamma'), (25, 'delta'))
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# (h) row-constructor IN where one column is NOT in the index: that column is
# skipped and only the indexed column contributes a clause. Dropping a column
# still leaves a superset of the original condition.
--let $qtitle = Q10 row IN with a non-indexed column
--let $w = (a, d) IN ((10, 300), (25, 500))
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# (i) row-constructor IN that matches nothing -- the distributed clauses each
# match rows, but no single row satisfies a pair. This is the case that would
# expose a distribution mistakenly treated as exact rather than as a weakening.
--let $qtitle = Q11 row IN with no matching pair
--let $w = (a, c) IN ((10, 'delta'), (25, 'gamma'))
--source suite/rocksdb/include/bitlsm_index_read_check.inc

# (j) negated row-constructor IN: not representable (there is no NOT_EQUAL
# CompareOp), so the translator must emit nothing for it rather than emit the
# positive form. Results must still be correct.
--let $qtitle = Q12 row NOT IN (must translate to nothing, stay correct)
--let $w = (a, c) NOT IN ((10, 'gamma'), (25, 'delta'))
--source suite/rocksdb/include/bitlsm_index_read_check.inc
