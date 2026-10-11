# One differential correctness check: whatever plan the optimizer chose (BitLSM
# or full scan), the row set must equal the full scan's (EXCEPT both ways = 0).
# Never dumps rows (the table is large). Params: $w, $qtitle.
--let $diff = `SELECT (SELECT COUNT(*) FROM (SELECT id,a,b,c,d FROM t_read WHERE $w EXCEPT SELECT id,a,b,c,d FROM t_read IGNORE INDEX(bi) WHERE $w) auto_minus_full) + (SELECT COUNT(*) FROM (SELECT id,a,b,c,d FROM t_read IGNORE INDEX(bi) WHERE $w EXCEPT SELECT id,a,b,c,d FROM t_read WHERE $w) full_minus_auto)`
--let $assert_text = $qtitle: chosen-plan row set identical to full scan
--let $assert_cond = $diff = 0
--source include/assert.inc
