# One differential correctness check for the M4b decision boundary. Whatever
# plan the optimizer chose (BitLSM or full scan), the returned row set must be
# byte-for-byte the full-scan row set: EXCEPT both ways = 0. Never dumps rows
# (the table is large); asserts a single scalar diff. Params: $w, $qtitle.
--let $diff = `SELECT (SELECT COUNT(*) FROM (SELECT id,a,b,c,d FROM t_read WHERE $w EXCEPT SELECT id,a,b,c,d FROM t_read IGNORE INDEX(bi) WHERE $w) auto_minus_full) + (SELECT COUNT(*) FROM (SELECT id,a,b,c,d FROM t_read IGNORE INDEX(bi) WHERE $w EXCEPT SELECT id,a,b,c,d FROM t_read WHERE $w) full_minus_auto)`
--let $assert_text = $qtitle: chosen-plan row set identical to full scan
--let $assert_cond = $diff = 0
--source include/assert.inc
