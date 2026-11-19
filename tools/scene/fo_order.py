# S103 FOB: the stage families' draw order, the game's (docs\research\s103\XFO.md G1-G5: canvas + skirts, the arena, the screens, the ring's posts / turnbuckles / covers / ropes, the props), checked from
# c_driver's `[c]   family order PANEL: tokens` lines. Shared by c_sheet.py (the brock_losswin captures: the ring is at rest there, no rows; split per key = the full order) and ring_posed_check.py (run 16's record: the ring posed, every piece).
# GAME_ORDER is the game's measured order (a constant, no game data). The fo_old_* panels are the planted old orders: each must FAIL the order check (when the ring is at rest only the two that move a whole row can).
#   from fo_order import run;  ok = run(log_text, diff_of)   # diff_of(panel) -> (pixels that differ from whole_plain by max channel diff > 12, pixels that differ at all)
import re

GAME_ORDER = ["canvas", "skirt", "arena", "screens", "post", "turnbuckle", "cover", "rope", "props"]   # props: optional last token (not every arena has the steps / desk mesh)
AT_REST_ORDER = ["arena", "screens", "ring_at_rest", "props"]   # ONLY when the per-key rest models do not add up to the ring set (an unknown arena): the whole set is one model in the late run. Normal at-rest frames (no ring rows) split per key and read GAME_ORDER
PLANTS = (("fo_old_skirts", "D1 canvas + skirts after the arena"), ("fo_old_screens", "D2 screens last"), ("fo_old_rank", "D3 ring pieces in the record's order"), ("fo_old_all", "D1+D2+D3 the old order"))
VISIBLE_AT_REST = ("fo_old_screens", "fo_old_all")


def tokens_of(log, panel):
    m = re.search(r"family order " + re.escape(panel) + r": ([a-z_ ]+)", log)
    return m.group(1).split() if m else None


def order_ok(toks, posed):
    want = GAME_ORDER if posed else AT_REST_ORDER
    return toks is not None and (toks == want or toks == want[:-1])


def run(log, diff_of):
    whole = tokens_of(log, "whole_plain")
    if whole is None:
        print("S103 FOB family order whole_plain: MISSING")
        return False
    posed = "post" in whole
    want = GAME_ORDER if posed else AT_REST_ORDER
    ok = order_ok(whole, posed)
    print(f"S103 FOB family order whole_plain ({'ring posed' if posed else 'ring at rest'}): {' '.join(whole)} -> {'as the game orders it' if ok else 'WRONG (the game: ' + ' '.join(want) + ')'}")
    for panel, what in PLANTS:
        toks = tokens_of(log, panel)
        if toks is None:
            ok = False
            print(f"S103 FOB planted order {panel}: MISSING")
            continue
        if not posed and panel not in VISIBLE_AT_REST:
            print(f"S103 FOB planted order {panel} ({what}): {' '.join(toks)} -> not observable with the ring at rest (checked on the posed record)")
        else:
            good = not order_ok(toks, posed)
            ok &= good
            print(f"S103 FOB planted order {panel} ({what}): {' '.join(toks)} -> {'FAILS the order check as required' if good else 'NOT DETECTED'}")
        n12, n1 = diff_of(panel)
        print(f"S103 FOB price {panel} ({what}) vs whole_plain: {n12} pixels differ (max channel diff > 12), {n1} pixels differ at all (> 0)")
    return ok
