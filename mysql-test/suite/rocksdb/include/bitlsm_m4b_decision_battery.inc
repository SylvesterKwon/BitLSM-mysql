# M4b decision-boundary battery. Sourced once with data in the memtable and once
# after a forced flush. Every EXPLAIN plan is machine-checked with a key probe
# (query_get_value + assert), and every chosen plan is proven to return exactly
# the full-scan row set (differential EXCEPT = 0). The estimate only changes the
# PLAN, never the RESULT.
#
# All predicates here are on NON-leading columns (they do not constrain 'a'), so
# they exercise the M4b synthetic estimator. (A leading-column predicate takes
# the stock range-cost path instead -- out of scope for M4b.) The estimator
# auto-selects BitLSM roughly when estimated selectivity < ~0.2, so single
# equalities (~0.1) win and broad ranges / multi-point IN-lists (~0.3+) lose.
# The probe queries select 'd' (not in bi) so the read is non-covering and
# re-fetches by PK; the select list is comma-free so query_get_value() parses it.

--echo #
--echo # (S1) SELECTIVE conjunction b=7 AND c=7 (sel ~ eq*eq=0.01) -> bi.
--let $k = query_get_value(EXPLAIN SELECT d FROM t_read WHERE b = 7 AND c = 7, key, 1)
--let $assert_text = selective conjunction (b=7 AND c=7) auto-selects bi
--let $assert_cond = "$k" = "bi"
--source include/assert.inc
--let $qtitle = S1 conjunction (b=7 AND c=7)
--let $w = b = 7 AND c = 7
--source suite/rocksdb/include/bitlsm_m4b_decision_check.inc

--echo #
--echo # (S2) SELECTIVE single equality c=7 (sel ~ eq=0.1) -> bi.
--let $k = query_get_value(EXPLAIN SELECT d FROM t_read WHERE c = 7, key, 1)
--let $assert_text = selective single equality (c=7) auto-selects bi
--let $assert_cond = "$k" = "bi"
--source include/assert.inc
--let $qtitle = S2 single equality (c=7)
--let $w = c = 7
--source suite/rocksdb/include/bitlsm_m4b_decision_check.inc

--echo #
--echo # (N1) NON-SELECTIVE range c>=1 (sel ~ range=0.33) prices ABOVE a full
--echo #      scan -> NOT bi (type=ALL). Same read path as S2, less selective ->
--echo #      opposite plan. This is the decision boundary.
--let $k = query_get_value(EXPLAIN SELECT d FROM t_read WHERE c >= 1, key, 1)
--let $assert_text = non-selective range (c>=1) does NOT auto-select bi
--let $assert_cond = "$k" != "bi"
--source include/assert.inc
--let $qtitle = N1 non-selective range (c>=1)
--let $w = c >= 1
--source suite/rocksdb/include/bitlsm_m4b_decision_check.inc

--echo #
--echo # (N2) NON-SELECTIVE IN-list c=1 OR c=3 OR c=5 (dead-simple estimator
--echo #      scores 3 points at 3*eq=0.3, above the ~0.2 threshold -> NOT bi).
--echo #      Exercises the n-point IN-list classification. (The real match is
--echo #      ~12% of rows; a correlation-aware estimate is M5.)
--let $k = query_get_value(EXPLAIN SELECT d FROM t_read WHERE c = 1 OR c = 3 OR c = 5, key, 1)
--let $assert_text = non-selective IN-list (c in 1,3,5) does NOT auto-select bi
--let $assert_cond = "$k" != "bi"
--source include/assert.inc
--let $qtitle = N2 IN-list (c=1 OR c=3 OR c=5)
--let $w = c = 1 OR c = 3 OR c = 5
--source suite/rocksdb/include/bitlsm_m4b_decision_check.inc

--echo #
--echo # (F) COST-FACTOR STEERING (bitlsm_index_cost_factor) moves the boundary
--echo #     both ways, at fixed data + fixed predicate -- proving the knob.
--echo #
--echo # (F-down) crank DOWN: selective c=7 (bi by default) flips AWAY to ALL.
SET SESSION bitlsm_index_cost_factor = 0.001;
--let $k = query_get_value(EXPLAIN SELECT d FROM t_read WHERE c = 7, key, 1)
--let $assert_text = cost_factor=0.001 flips selective c=7 away from bi
--let $assert_cond = "$k" != "bi"
--source include/assert.inc
SET SESSION bitlsm_index_cost_factor = DEFAULT;

--echo #
--echo # (F-up) crank UP: non-selective c>=1 (ALL by default) flips TO bi.
SET SESSION bitlsm_index_cost_factor = 1000000;
--let $k = query_get_value(EXPLAIN SELECT d FROM t_read WHERE c >= 1, key, 1)
--let $assert_text = cost_factor=1e6 flips non-selective c>=1 to bi
--let $assert_cond = "$k" = "bi"
--source include/assert.inc
--echo # correctness invariant even when the knob forces bi onto c>=1:
--let $qtitle = F-up c>=1 under cost_factor=1e6
--let $w = c >= 1
--source suite/rocksdb/include/bitlsm_m4b_decision_check.inc
SET SESSION bitlsm_index_cost_factor = DEFAULT;
