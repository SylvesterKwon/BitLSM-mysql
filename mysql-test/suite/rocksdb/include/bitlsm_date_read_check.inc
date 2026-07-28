# Differential check for one bitlsm read on the DATE table t_date.
#
# Mirrors bitlsm_index_read_check.inc but for t_date's column list. Asserts the
# FORCE INDEX(bi) row set is identical to the same query resolved by a full
# table scan (IGNORE INDEX(bi)). A DATE comparand packed differently from the
# way Field_newdate stores the column does not crash and does not change the
# translated STRING -- it silently prunes away rows that should have matched.
# This assertion is the only thing that catches it.
#
# Parameters:
#   $qtitle - human description of the query
#   $w      - WHERE predicate over t_date's bi columns (a, d)

# payload is not in bi, so selecting it makes the index non-covering and pushes
# the WHERE through ICP -- the point the translator reads.
--echo # $qtitle
--eval SELECT id, a, d, payload FROM t_date FORCE INDEX(bi) WHERE $w ORDER BY id

# Symmetric difference must be empty. EXCEPT is NULL-aware (NULL matches NULL),
# so rows with a NULL index column cancel correctly. id is unique.
--let $diff = `SELECT (SELECT COUNT(*) FROM (SELECT id,a,d,payload FROM t_date FORCE INDEX(bi) WHERE $w EXCEPT SELECT id,a,d,payload FROM t_date IGNORE INDEX(bi) WHERE $w) force_minus_full) + (SELECT COUNT(*) FROM (SELECT id,a,d,payload FROM t_date IGNORE INDEX(bi) WHERE $w EXCEPT SELECT id,a,d,payload FROM t_date FORCE INDEX(bi) WHERE $w) full_minus_force)`
--let $assert_text = $qtitle: FORCE INDEX(bi) row set identical to full scan
--let $assert_cond = $diff = 0
--source include/assert.inc
