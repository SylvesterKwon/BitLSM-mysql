# Differential check for one bitlsm index read (M3b-1).
#
# A BITLSM_INDEX writes no secondary-key entries, so a normal range scan finds
# nothing. When the index is chosen, the engine instead full-scans the PK data
# CF and lets ICP filter each row against the pushed WHERE. This helper asserts
# that the FORCE INDEX(bi) row set is byte-for-byte identical to the same query
# resolved by a full table scan (IGNORE INDEX(bi)), for the SAME predicate.
#
# Parameters:
#   $qtitle - human description of the query
#   $w      - WHERE predicate over the bi columns (a, b, c)
#
# The FORCE INDEX(bi) rows are recorded (so a divergence is visible in the diff)
# and the set equality is machine-enforced via include/assert.inc (so a
# recorded-but-wrong result cannot pass silently).

# Selecting the non-indexed payload column d makes bi non-covering, so the
# server pushes the WHERE as an index condition (ICP) -- exercising the engine's
# check_index_cond() filter -- rather than post-filtering a covered row.
--echo # $qtitle
--eval SELECT id, a, b, c, d FROM t_read FORCE INDEX(bi) WHERE $w ORDER BY id

# Symmetric difference between the index path and the full scan must be empty.
# EXCEPT is NULL-aware (NULL matches NULL), so rows with NULL index columns
# cancel correctly. id is unique, so no row is double-counted.
--let $diff = `SELECT (SELECT COUNT(*) FROM (SELECT id,a,b,c,d FROM t_read FORCE INDEX(bi) WHERE $w EXCEPT SELECT id,a,b,c,d FROM t_read IGNORE INDEX(bi) WHERE $w) force_minus_full) + (SELECT COUNT(*) FROM (SELECT id,a,b,c,d FROM t_read IGNORE INDEX(bi) WHERE $w EXCEPT SELECT id,a,b,c,d FROM t_read FORCE INDEX(bi) WHERE $w) full_minus_force)`
--let $assert_text = $qtitle: FORCE INDEX(bi) row set identical to full scan
--let $assert_cond = $diff = 0
--source include/assert.inc
