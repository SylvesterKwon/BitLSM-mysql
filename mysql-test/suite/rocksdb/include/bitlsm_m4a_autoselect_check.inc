# Differential check for one AUTO-SELECTED bitlsm index read (M4a).
#
# Unlike the M3b check, this query carries NO FORCE INDEX(bi): the optimizer is
# expected to have chosen bi on cost. The left side lets the optimizer choose;
# the right side pins a full scan with IGNORE INDEX(bi). The two row sets must
# be byte-for-byte identical.
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
