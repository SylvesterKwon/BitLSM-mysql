# Differential check for one bitlsm read on t_date: FORCE INDEX(bi) must equal
# the full scan (IGNORE INDEX(bi)). This is what catches a mis-packed DATE.
#
# Parameters:
#   $qtitle - human description of the query
#   $w      - WHERE predicate over t_date's bi columns (a, d)

# payload is not in bi: non-covering, so the WHERE is pushed via ICP.
--echo # $qtitle
--eval SELECT id, a, d, payload FROM t_date FORCE INDEX(bi) WHERE $w ORDER BY id

# Symmetric difference must be empty. EXCEPT is NULL-aware (NULL matches NULL),
# so rows with a NULL index column cancel correctly. id is unique.
--let $diff = `SELECT (SELECT COUNT(*) FROM (SELECT id,a,d,payload FROM t_date FORCE INDEX(bi) WHERE $w EXCEPT SELECT id,a,d,payload FROM t_date IGNORE INDEX(bi) WHERE $w) force_minus_full) + (SELECT COUNT(*) FROM (SELECT id,a,d,payload FROM t_date IGNORE INDEX(bi) WHERE $w EXCEPT SELECT id,a,d,payload FROM t_date FORCE INDEX(bi) WHERE $w) full_minus_force)`
--let $assert_text = $qtitle: FORCE INDEX(bi) row set identical to full scan
--let $assert_cond = $diff = 0
--source include/assert.inc
