# Differential check for one auto-selected bitlsm read: no FORCE INDEX(bi), the
# optimizer must choose bi on cost, and the rows must equal IGNORE INDEX(bi).
#
# Parameters:
#   $qtitle - human description of the query
#   $w      - WHERE predicate over the bi columns (a, b, c)

--echo # $qtitle
--eval SELECT id, a, b, c, d FROM t_read WHERE $w ORDER BY id

--let $diff = `SELECT (SELECT COUNT(*) FROM (SELECT id,a,b,c,d FROM t_read WHERE $w EXCEPT SELECT id,a,b,c,d FROM t_read IGNORE INDEX(bi) WHERE $w) auto_minus_full) + (SELECT COUNT(*) FROM (SELECT id,a,b,c,d FROM t_read IGNORE INDEX(bi) WHERE $w EXCEPT SELECT id,a,b,c,d FROM t_read WHERE $w) full_minus_auto)`
--let $assert_text = $qtitle: auto-selected row set identical to full scan
--let $assert_cond = $diff = 0
--source include/assert.inc
