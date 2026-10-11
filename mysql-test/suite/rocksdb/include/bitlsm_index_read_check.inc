# Differential check for one bitlsm index read: FORCE INDEX(bi) must equal the
# full scan (IGNORE INDEX(bi)). The rows are recorded so a divergence shows in
# the diff; set equality is enforced via include/assert.inc.
#
# Parameters:
#   $qtitle - human description of the query
#   $w      - WHERE predicate over the bi columns (a, b, c)

# d is not in bi: non-covering, so the WHERE is pushed as an index condition.
--echo # $qtitle
--eval SELECT id, a, b, c, d FROM t_read FORCE INDEX(bi) WHERE $w ORDER BY id

# Symmetric difference between the index path and the full scan must be empty.
# EXCEPT is NULL-aware (NULL matches NULL), so rows with NULL index columns
# cancel correctly. id is unique, so no row is double-counted.
--let $diff = `SELECT (SELECT COUNT(*) FROM (SELECT id,a,b,c,d FROM t_read FORCE INDEX(bi) WHERE $w EXCEPT SELECT id,a,b,c,d FROM t_read IGNORE INDEX(bi) WHERE $w) force_minus_full) + (SELECT COUNT(*) FROM (SELECT id,a,b,c,d FROM t_read IGNORE INDEX(bi) WHERE $w EXCEPT SELECT id,a,b,c,d FROM t_read FORCE INDEX(bi) WHERE $w) full_minus_force)`
--let $assert_text = $qtitle: FORCE INDEX(bi) row set identical to full scan
--let $assert_cond = $diff = 0
--source include/assert.inc
