# One BitLSM query-assembly check: run a SELECT through the bitlsm read path
# (FORCE INDEX(bi), selecting the non-indexed d so the WHERE is pushed as an
# index condition), read the assembled query via rocksdb_bitlsm_last_query,
# echo it into the .result and assert it equals $expected.
#
# Parameters:
#   $t        - table to read (t_asm, or t_asm_ord for the ORDERED variant)
#   $w        - WHERE predicate over the bi columns
#   $expected - exact BitLSMQuery::ToString() ("" = nothing representable)

--disable_result_log
--eval SELECT id, a, b, c, d FROM $t FORCE INDEX(bi) WHERE $w
--enable_result_log
--let $got = query_get_value(SHOW SESSION STATUS LIKE 'rocksdb_bitlsm_last_query', Value, 1)
--echo # WHERE $w
--echo #   assembled: [$got]
if ($got != $expected) {
  --echo FAILED: assembled [$got] != expected [$expected] for WHERE [$w]
  --die BitLSM query assembly mismatch
}
