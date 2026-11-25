r"""qfnfacts selftest: a HAND-WRITTEN synthetic listing (invented addresses 0x10000000.., the recompiler's comment / label / goto / switch
shape; no game code) with one planted positive and one planted false positive per fact, built end to end into a temp db.

    py -I tools\q\qfnfacts_selftest.py
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qfnfacts as F  # noqa: E402

FIXTURE = r'''#include "x.h"

DEFINE_REX_FUNC(sub_10000000) {
	REX_FUNC_PROLOGUE();
	// li r30,4
	// addi r31,r3,8
loc_10000008:
	// lwz r3,0(r31)
	// addic. r30,r30,-1
	// addi r31,r31,24
	// bne 0x10000008
	if (!cr0.eq) goto loc_10000008;
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10000100) {
	REX_FUNC_PROLOGUE();
	// cmpwi cr6,r3,0
	// bne cr6,0x1000010c
	if (!cr6.eq) goto loc_1000010C;
loc_10000108:
	// blr
	return;
loc_1000010C:
	// li r3,1
	// b 0x10000108
	goto loc_10000108;
}

DEFINE_REX_FUNC(sub_10000200) {
	REX_FUNC_PROLOGUE();
	// cmplwi cr6,r3,2
	// bgt cr6,0x10000230
	if (cr6.gt) goto loc_10000230;
	// lis r12,4096
	// rlwinm r0,r3,2,0,29
	// addi r12,r12,0
	// lwzx r0,r12,r0
	// mtctr r0
	// bctr
	switch (ctx.r3.u32) {
	case 0:
		goto loc_10000220;
	case 1:
		goto loc_10000228;
	case 2:
		goto loc_10000220;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_10000220:
	// li r3,1
	// blr
	return;
loc_10000228:
	// li r3,2
	// blr
	return;
loc_10000230:
	// li r3,0
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10000300) {
	REX_FUNC_PROLOGUE();
	// lwz r11,0(r3)
	// lwz r10,8(r11)
	// mtctr r10
	// bctr
	REX_CALL_INDIRECT_FUNC(ctr.u32);
	return;
}

DEFINE_REX_FUNC(sub_10000400) {
	REX_FUNC_PROLOGUE();
	// cmpwi cr6,r4,1
	// beq cr6,0x10000414
	if (cr6.eq) goto loc_10000414;
	// cmpwi cr6,r4,2
	// beq cr6,0x1000041c
	if (cr6.eq) goto loc_1000041C;
	// b 0x10000424
	goto loc_10000424;
loc_10000414:
	// li r4,0
	// blr
	return;
loc_1000041C:
	// li r3,1
	// blr
	return;
loc_10000424:
	// cmpwi cr6,r4,3
	// beq cr6,0x1000041c
	if (cr6.eq) goto loc_1000041C;
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10000500) {
	REX_FUNC_PROLOGUE();
	// lwz r11,0(r3)
	// cmpwi cr6,r11,1
	// beq cr6,0x10000524
	if (cr6.eq) goto loc_10000524;
	// lwz r11,4(r3)
	// cmpwi cr6,r11,2
	// beq cr6,0x10000524
	if (cr6.eq) goto loc_10000524;
	// lwz r11,8(r3)
	// cmpwi cr6,r11,3
	// beq cr6,0x10000524
	if (cr6.eq) goto loc_10000524;
loc_10000524:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10000600) {
	REX_FUNC_PROLOGUE();
	// mr r31,r3
	// mr r30,r4
	// bl 0x10000a00
	sub_10000A00(ctx, base);
	// clrlwi r11,r30,31
	// mr r3,r31
	// cmplwi cr6,r11,0
	// beq cr6,0x10000624
	if (cr6.eq) goto loc_10000624;
	// bl 0x10000b00
	sub_10000B00(ctx, base);
	// mr r3,r31
loc_10000624:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10000700) {
	REX_FUNC_PROLOGUE();
	// mr r31,r3
	// bl 0x10000a00
	sub_10000A00(ctx, base);
	// clrlwi r11,r4,31
	// cmplwi cr6,r11,0
	// beq cr6,0x10000718
	if (cr6.eq) goto loc_10000718;
	// mr r3,r31
loc_10000718:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10000800) {
	REX_FUNC_PROLOGUE();
	// cmpwi cr6,r3,0
	// beq cr6,0x1000080c
	if (cr6.eq) goto loc_1000080C;
	// li r3,5
loc_1000080C:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10000900) {
	REX_FUNC_PROLOGUE();
	// cmpwi cr6,r3,0
	// beq cr6,0x1000090c
	if (cr6.eq) goto loc_1000090C;
	// li r3,7
loc_1000090C:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10000A00) {
	REX_FUNC_PROLOGUE();
	// cmpwi cr6,r3,0
	// beq cr6,0x10000a0c
	if (cr6.eq) goto loc_10000A0C;
	// li r3,5
loc_10000A0C:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10000B00) {
	REX_FUNC_PROLOGUE();
	// cmpwi cr6,r3,0
	// beq cr6,0x10000b0c
	if (cr6.eq) goto loc_10000B0C;
	// li r4,5
loc_10000B0C:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10000D00) {
	REX_FUNC_PROLOGUE();
	// mr r31,r3
	// mr r30,r4
	// bl 0x10000a00
	sub_10000A00(ctx, base);
	// clrlwi r11,r30,31
	// mr r3,r31
	// cmplwi cr6,r11,0
	// beq cr6,0x10000d24
	if (cr6.eq) goto loc_10000D24;
	// bl 0x10000c00
	sub_10000C00(ctx, base);
	// mr r3,r31
loc_10000D24:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001000) {
	REX_FUNC_PROLOGUE();
	// li r29,0
	// li r28,0
loc_10001008:
	// li r30,0
loc_1000100C:
	// cmpwi cr6,r30,8
	// bge cr6,0x10001030
	if (!cr6.lt) goto loc_10001030;
	// mr r4,r30
	// bl 0x10000a00
	sub_10000A00(ctx, base);
	// cmplwi cr6,r30,5
	// bgt cr6,0x10001028
	if (cr6.gt) goto loc_10001028;
	// li r3,0
loc_10001028:
	// addi r30,r30,1
	// b 0x1000100c
	goto loc_1000100C;
loc_10001030:
	// stw r3,0(r28)
	// addi r28,r28,12
	// addi r29,r29,1
	// cmpwi cr6,r29,3
	// blt cr6,0x10001008
	if (cr6.lt) goto loc_10001008;
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001100) {
	REX_FUNC_PROLOGUE();
	// li r11,4
	// mtctr r11
loc_10001108:
	// lvx128 v0,r0,r10
	// addi r10,r10,16
	// bdnz 0x10001108
	if (--ctr.u64 != 0) goto loc_10001108;
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001200) {
	REX_FUNC_PROLOGUE();
	// addis r11,r4,-4096
	// addi r11,r11,-1
	// li r10,0
	// li r9,0
	// li r8,0
	// li r7,0
	// li r6,0
	// li r5,0
	// cmplwi cr6,r11,2
	// bgt cr6,0x10001250
	if (cr6.gt) goto loc_10001250;
	// lis r12,4096
	// rlwinm r0,r11,2,0,29
	// addi r12,r12,0
	// lwzx r0,r12,r0
	// mtctr r0
	// bctr
	switch (ctx.r11.u32) {
	case 0:
		goto loc_10001240;
	case 1:
		goto loc_10001248;
	case 2:
		goto loc_10001240;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_10001240:
	// li r3,1
	// blr
	return;
loc_10001248:
	// li r3,2
	// blr
	return;
loc_10001250:
	// li r3,0
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001300) {
	REX_FUNC_PROLOGUE();
	// cmpwi cr6,r4,0
	// beq cr6,0x10001310
	if (cr6.eq) goto loc_10001310;
	// lwz r11,0(r3)
	// b 0x10001314
	goto loc_10001314;
loc_10001310:
	// li r11,0
loc_10001314:
	// addi r11,r11,-1
	// cmplwi cr6,r11,1
	// lbz r9,0(r3)
	// lbz r8,1(r3)
	// lbz r7,2(r3)
	// bgt cr6,0x10001348
	if (cr6.gt) goto loc_10001348;
	// lis r12,4096
	// rlwinm r0,r11,2,0,29
	// addi r12,r12,0
	// lwzx r0,r12,r0
	// mtctr r0
	// bctr
	switch (ctx.r11.u32) {
	case 0:
		goto loc_10001344;
	case 1:
		goto loc_10001348;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_10001344:
	// li r3,1
loc_10001348:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001400) {
	REX_FUNC_PROLOGUE();
	// cmpwi cr6,r4,0
	// beq cr6,0x10001410
	if (cr6.eq) goto loc_10001410;
	// lwz r11,0(r3)
	// b 0x10001414
	goto loc_10001414;
loc_10001410:
	// li r11,0
loc_10001414:
	// addi r11,r11,-1
	// cmplwi cr6,r11,5
	// beq cr6,0x10001434
	if (cr6.eq) goto loc_10001434;
	// cmplwi cr6,r11,7
	// beq cr6,0x10001434
	if (cr6.eq) goto loc_10001434;
	// cmplwi cr6,r11,9
	// beq cr6,0x10001434
	if (cr6.eq) goto loc_10001434;
	// li r3,0
loc_10001434:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001500) {
	REX_FUNC_PROLOGUE();
	// addi r3,r31,1056
	// bl 0x10000a00
	sub_10000A00(ctx, base);
	// cmplwi cr6,r3,1
	// bgt cr6,0x10001530
	if (cr6.gt) goto loc_10001530;
	// lis r12,4096
	// rlwinm r0,r3,2,0,29
	// addi r12,r12,0
	// lwzx r0,r12,r0
	// mtctr r0
	// bctr
	switch (ctx.r3.u32) {
	case 0:
		goto loc_10001528;
	case 1:
		goto loc_10001530;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_10001528:
	// li r3,1
	// blr
	return;
loc_10001530:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001600) {
	REX_FUNC_PROLOGUE();
	// cmplwi cr6,r3,1
	// bgt cr6,0x10001628
	if (cr6.gt) goto loc_10001628;
	// lis r12,4096
	// rlwinm r0,r3,2,0,29
	// addi r12,r12,256
	// lwzx r0,r12,r0
	// mtctr r0
	// bctr
	switch (ctx.r3.u32) {
	case 0:
		goto loc_10001620;
	case 1:
		goto loc_10001628;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_10001620:
	// li r3,1
	// blr
	return;
loc_10001628:
	// li r3,0
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001700) {
	REX_FUNC_PROLOGUE();
	// cmplwi cr6,r3,1
	// bgt cr6,0x10001728
	if (cr6.gt) goto loc_10001728;
	// lis r12,4096
	// rlwinm r0,r3,2,0,29
	// addi r12,r12,512
	// lwzx r0,r12,r0
	// mtctr r0
	// bctr
	switch (ctx.r3.u32) {
	case 0:
		goto loc_10001728;
	case 1:
		goto loc_10001720;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_10001720:
	// li r3,1
	// blr
	return;
loc_10001728:
	// li r3,0
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001800) {
	REX_FUNC_PROLOGUE();
	// cmplwi cr6,r3,1
	// bgt cr6,0x10001828
	if (cr6.gt) goto loc_10001828;
	// lis r12,4096
	// rlwinm r0,r3,2,0,29
	// addi r12,r12,768
	// lwzx r0,r12,r0
	// mtctr r0
	// bctr
	switch (ctx.r3.u32) {
	case 0:
		goto loc_10001820;
	case 1:
		goto loc_10001828;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_10001820:
	// li r3,1
	// blr
	return;
loc_10001828:
	// li r3,0
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001900) {
	REX_FUNC_PROLOGUE();
	// cmpwi cr6,r4,119
	// bne cr6,0x1000190c
	if (!cr6.eq) goto loc_1000190C;
	// lwz r4,36(r3)
loc_1000190C:
	// cmplwi cr6,r4,1
	// bgt cr6,0x1000192c
	if (cr6.gt) goto loc_1000192C;
	// lis r12,4096
	// rlwinm r0,r4,2,0,29
	// addi r12,r12,0
	// lwzx r0,r12,r0
	// mtctr r0
	// bctr
	switch (ctx.r4.u32) {
	case 0:
		goto loc_1000192C;
	case 1:
		goto loc_1000192C;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_1000192C:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001A00) {
	REX_FUNC_PROLOGUE();
	// cmpwi cr6,r4,0
	// beq cr6,0x10001a10
	if (cr6.eq) goto loc_10001A10;
	// addi r11,r5,-1
	// b 0x10001a14
	goto loc_10001A14;
loc_10001A10:
	// addi r11,r5,-2
loc_10001A14:
	// cmplwi cr6,r11,1
	// bgt cr6,0x10001a34
	if (cr6.gt) goto loc_10001A34;
	// lis r12,4096
	// rlwinm r0,r11,2,0,29
	// addi r12,r12,0
	// lwzx r0,r12,r0
	// mtctr r0
	// bctr
	switch (ctx.r11.u32) {
	case 0:
		goto loc_10001A34;
	case 1:
		goto loc_10001A34;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_10001A34:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001B00) {
	REX_FUNC_PROLOGUE();
	// li r29,0
loc_10001B04:
	// cmplwi cr6,r29,1
	// bgt cr6,0x10001b24
	if (cr6.gt) goto loc_10001B24;
	// lis r12,4096
	// rlwinm r0,r29,2,0,29
	// addi r12,r12,0
	// lwzx r0,r12,r0
	// mtctr r0
	// bctr
	switch (ctx.r29.u32) {
	case 0:
		goto loc_10001B24;
	case 1:
		goto loc_10001B24;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_10001B24:
	// addi r29,r29,1
	// cmpwi cr6,r29,3
	// blt cr6,0x10001b04
	if (cr6.lt) goto loc_10001B04;
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001C00) {
	REX_FUNC_PROLOGUE();
	// cmplwi cr6,r20,7
	// bgt cr6,0x10001c28
	if (cr6.gt) goto loc_10001C28;
	// clrlwi r11,r4,30
	// addi r20,r11,7
	// lis r12,4096
	// rlwinm r0,r20,2,0,29
	// addi r12,r12,0
	// lwzx r0,r12,r0
	// mtctr r0
	// bctr
	switch (ctx.r20.u32) {
	case 0:
		goto loc_10001C28;
	case 1:
		goto loc_10001C28;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_10001C28:
	// blr
	return;
}

DEFINE_REX_FUNC(sub_10001D00) {
	REX_FUNC_PROLOGUE();
	// addi r11,r30,-39
	// cmplwi cr6,r11,1
	// bgt cr6,0x10001d28
	if (cr6.gt) goto loc_10001D28;
	// addi r11,r30,-39
	// lis r12,4096
	// rlwinm r0,r11,2,0,29
	// addi r12,r12,0
	// lwzx r0,r12,r0
	// mtctr r0
	// bctr
	switch (ctx.r11.u32) {
	case 0:
		goto loc_10001D28;
	case 1:
		goto loc_10001D28;
	default:
		__builtin_trap(); // Switch case out of range
	}
loc_10001D28:
	// blr
	return;
}
'''

FAILS = []


def check(what, cond):
    print('%s  %s' % ('ok  ' if cond else 'FAIL', what))
    if not cond:
        FAILS.append(what)


def main():
    d = tempfile.mkdtemp(prefix='qfnfacts_')
    try:
        gen = os.path.join(d, 'gen')
        os.makedirs(gen)
        with open(os.path.join(gen, 'x_recomp.0.cpp'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(FIXTURE)
        out = os.path.join(d, 'fnfacts.s1.db')
        F.build(gen, out, jobs=1, quiet=True)
        con = F.connect(out)

        def rows(sql, *a):
            return con.execute(sql, a).fetchall()
        check('every body parsed (27 functions)', rows('SELECT COUNT(*) FROM fn')[0][0] == 27)
        # loop
        check('loop: the stride loop of sub_10000000 (head 10000008, latch 10000014)',
              rows('SELECT head, latch FROM loop WHERE func=0x10000000') == [(0x10000008, 0x10000014)])
        ind = {r[0]: r[1:] for r in rows('SELECT reg, kind, step, init, bound FROM loop_ind WHERE func=0x10000000')}
        check('loop: r31 is a ptr induction of step 24', ind.get('r31', ())[:2] == ('ptr', 24))
        check('loop: r30 is an idx induction of step -1 from 4', ind.get('r30', ())[:3] == ('idx', -1, 4))
        check('loop: r3 (reloaded every pass) is no induction', 'r3' not in ind)
        check('loop FALSE POSITIVE: the backward jump of sub_10000100 to a return is not a loop',
              rows('SELECT COUNT(*) FROM loop WHERE func=0x10000100')[0][0] == 0)
        # switch
        sw = rows('SELECT site, reg, n_cases, n_targets, lo, hi, bias, deflt FROM switch WHERE func=0x10000200')
        check('switch: sub_10000200 at its bctr, 3 cases -> 2 targets, 0..2, default 10000230',
              sw == [(0x1000021C, 'r3', 3, 2, 0, 2, 0, 0x10000230)])
        check('switch: case 1 -> 10000228', rows('SELECT target FROM switch_case WHERE func=0x10000200 AND value=1') == [(0x10000228,)])
        check('switch FALSE POSITIVE: the indirect tail bctr of sub_10000300 is no switch',
              rows('SELECT COUNT(*) FROM switch WHERE func=0x10000300')[0][0] == 0)
        # chain
        ch = rows('SELECT val, reg, n, n_eq FROM chain WHERE func=0x10000400')
        check('chain: arg r4 against 1, 2, 3 (the write of r4 on ANOTHER path does not break it)', ch == [('arg:r4', 'r4', 3, 3)])
        check('chain: its values', [r[0] for r in rows('SELECT value FROM chain_val WHERE func=0x10000400 ORDER BY site')] == [1, 2, 3])
        check('chain FALSE POSITIVE: r11 reloaded before each compare (three values) is no chain',
              rows('SELECT COUNT(*) FROM chain WHERE func=0x10000500')[0][0] == 0)
        # argbit
        check('argbit: sub_10000600 tests r4 & 1 (copied to r30 across a call)',
              rows('SELECT arg, mask, site, how FROM argbit WHERE func=0x10000600') == [(4, 1, 0x1000060C, 'cmp')])
        check('argbit FALSE POSITIVE: r4 after a call (killed) is not the entry r4',
              rows('SELECT COUNT(*) FROM argbit WHERE func=0x10000700')[0][0] == 0)
        # clones
        h = {r[0]: r[1:] for r in rows('SELECT func, h_exact, h_noimm, h_nocall, h_shape FROM fn')}
        check('clones: sub_10000800 and sub_10000A00 are exact clones (branch targets relative)', h[0x10000800][0] == h[0x10000A00][0])
        check('clones: sub_10000900 differs from sub_10000800 only by an immediate (h_noimm equal, h_exact not)',
              h[0x10000900][1] == h[0x10000800][1] and h[0x10000900][0] != h[0x10000800][0])
        check('clones: sub_10000D00 differs from sub_10000600 only by a callee (h_nocall equal, h_exact not)',
              h[0x10000D00][2] == h[0x10000600][2] and h[0x10000D00][0] != h[0x10000600][0])
        check('clones FALSE POSITIVE: sub_10000B00 (another register) is in no group of sub_10000800, not even h_shape',
              h[0x10000B00][3] != h[0x10000800][3])
        # S110 review: planted twins, each one failed on the code as landed (542baa591)
        lp = {r[0]: r[1:] for r in rows('SELECT head, latch, n_body FROM loop WHERE func=0x10001000')}
        ind_in = {r[0]: r[1:] for r in rows('SELECT reg, kind, step, init, bound FROM loop_ind WHERE func=0x10001000 AND head=0x1000100C')}
        ind_out = {r[0]: r[1:] for r in rows('SELECT reg, kind, step, init, bound FROM loop_ind WHERE func=0x10001000 AND head=0x10001008')}
        check('nested loops: the inner loop is its own 9 instructions, not the outer path (head 1000100C, latch 1000102C)',
              lp.get(0x1000100C) == (0x1000102C, 9))
        check('nested loops: the inner loop holds only r30 (passed in r4: arg; from 0; exit test 8, the in-body range test 5 is no bound)',
              ind_in == {'r30': ('arg', 1, 0, 8)})
        check('nested loops: the outer loop holds r28 (ptr +12) and r29 (idx +1, bound 3), not the inner r30',
              ind_out == {'r28': ('ptr', 12, 0, None), 'r29': ('idx', 1, 0, 3)})
        ind = {r[0]: r[1:] for r in rows('SELECT reg, kind, step, init FROM loop_ind WHERE func=0x10001100')}
        check('loop: r10 read by lvx128 (an indexed vector load) is a ptr induction; ctr from 4',
              ind.get('r10') == ('ptr', 16, None) and ind.get('ctr') == ('ctr', -1, 4))
        check('switch: an addis + addi bias 8 instructions before the bound test (values 0x10000001..0x10000003, default 10001250)',
              rows('SELECT lo, hi, bias, deflt FROM switch WHERE func=0x10001200') == [(0x10000001, 0x10000003, 0x10000001, 0x10001250)])
        check('switch: a bias after a join of a constant path and a load path; the default branch 4 instructions after the bound test',
              rows('SELECT lo, hi, bias, deflt FROM switch WHERE func=0x10001300') == [(1, 2, 1, 0x10001348)])
        check('switch: an addi into r3 BEFORE a call is no bias of a switch on the call result',
              rows('SELECT lo, hi, bias FROM switch WHERE func=0x10001500') == [(0, 1, 0)])
        check('switch: two paths into the switch, a load and the entry value, neither adding: bias 0 (not unknown)',
              rows('SELECT lo, hi, bias FROM switch WHERE func=0x10001900') == [(0, 1, 0)])
        check('switch: two paths adding -1 and -2: the bias and the values are unknown (NULL), the indexes stay',
              rows('SELECT lo, hi, bias FROM switch WHERE func=0x10001A00') == [(None, None, None)]
              and rows('SELECT idx, value FROM switch_case WHERE func=0x10001A00 ORDER BY idx') == [(0, None), (1, None)])
        check('switch: a loop counter switched on (its `addi r29,r29,1` comes around the loop): bias 0, not unknown',
              rows('SELECT lo, hi, bias FROM switch WHERE func=0x10001B00') == [(0, 1, 0)])
        check('switch: a compare of r20 BEFORE r20 is rewritten (`addi r20,r11,7`) is no bound test: no default; bias -7 from r11',
              rows('SELECT lo, hi, bias, deflt FROM switch WHERE func=0x10001C00') == [(-7, -6, -7, None)])
        check('switch: the index recomputed by the same `addi r11,r30,-39` after the bound test keeps that test (default 10001D28)',
              rows('SELECT lo, hi, bias, deflt FROM switch WHERE func=0x10001D00') == [(39, 40, 39, 0x10001D28)])
        check('chain: a value made after a join of a constant path and a load path (def:10001414 against 5, 7, 9)',
              rows('SELECT val, reg, n, n_eq FROM chain WHERE func=0x10001400') == [('def:10001414', 'r11', 3, 3)])
        check('clones: a switch with the same shape but another case -> target map is no h_noimm clone (another table address is)',
              h[0x10001600][1] == h[0x10001800][1] and h[0x10001600][1] != h[0x10001700][1])
        # the readers answer
        check('show answers for a planted function', F.show(con, 0x10000600) == 0)
        check('stats answers', F.stats(con) == 0)
        con.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print('qfnfacts selftest: %s' % ('PASS' if not FAILS else 'FAIL (%d)' % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
