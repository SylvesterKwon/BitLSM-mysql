# One BitLSM query-assembly check (M3b-2).
#
# Runs a SELECT that routes through the bitlsm index read path (FORCE INDEX(bi),
# selecting the non-indexed payload d so the WHERE is pushed as an index
# condition), then reads back the ToString() of the query the engine assembled
# from that pushed WHERE via the debug session status var
# rocksdb_bitlsm_last_query, echoes it (so the recorded .result is the golden
# assertion), and hard-asserts it equals $expected.
#
# The assembled query is only a pruning HINT and does not affect result rows in
# M3b-2, so this verifies TRANSLATION COMPLETENESS (the exact string) that the
# differential bitlsm_index_read test cannot see.
#
# Parameters:
#   $w        - WHERE predicate over the bi columns (a, b, c)
#   $expected - the exact BitLSMQuery::ToString() the translator must produce
#               ("" means no representable pruning condition)

--disable_result_log
--eval SELECT id, a, b, c, d FROM t_asm FORCE INDEX(bi) WHERE $w
--enable_result_log
--let $got = query_get_value(SHOW SESSION STATUS LIKE 'rocksdb_bitlsm_last_query', Value, 1)
--echo # WHERE $w
--echo #   assembled: [$got]
if ($got != $expected) {
  --echo FAILED: assembled [$got] != expected [$expected] for WHERE [$w]
  --die BitLSM query assembly mismatch
}
