r"""q rstdiff, inherited state (S111 RSTCOVER-B): a state no draw kind's window sets, read from EVERY setter of it in the game.

The colour write mask (recorder slots 13-17: cache words +92 all, +96 / +100 / +104 / +108 the four channels) and the stencil
enable (slot 5: word +28) are set through their recorder wrappers by a handful of functions (q's call edges) and by nothing a
draw kind runs: the op-80 per-category reset (sub_82C26ED0, its recorder slot 38 sub_82C22E88) writes neither, and no effect
pass carries either (q rstdiff's effect-file line). So the state at any draw outside those functions is the recorder's start
value (its initialiser sub_82C22F88 writes the cache words) carried through every setter, PROVIDED each setter leaves the words
as it found them: on every path to its return, the last call of each slot passes the start value, or the value the recorder's
own getter of the same word returned earlier, before any set of that word on the path (a save / restore; a getter read after
the function's own set returns that set value, so it is no save). The paths are read with the same forward data flow as the
kind windows (qrstdiff), with three more things tracked: a getter's result (a token for its word), the stack slots
(`stw rS,N(r1)` / `lwz rD,N(r1)`) and the words of the function's first argument (`stw` / `lwz` at N(r3 as it came in)), so a
value saved at the top and restored at the bottom, or saved by one function into a struct and restored by its PAIR from the
same struct, is recognised. A setter that leaves another value on some path is NAMED, and the state stays UNSETTLED.
Device value from the words: the mask methods (0x82C23910 slot 13; 0x82C23980 .. 0x82C23B60 slots 14-17) record render
state 53 = 15 when the all-word and the four channel words are 1 (slot 13: 15 for a non-zero argument; 14-17: bits 1 / 2 / 4
for +96 / +100 / +104 while +92 is non-zero, bit 8 for +108 whatever +92 holds: S111 GINHERIT review); the stencil method
0x82C23490 records render state 27 = its argument.
A kind whose game window is in a setter's own function draws inside that setter's set section (S111 RSTCOVER-A's circle blobs
in 0x82813228, which sets the channel words +96 / +100 the kind windows do not read): its field is not filled, the setter is
named and the row stays UNSETTLED (fill's `fns`). When the kind gives its window's end, a setter that calls none of the state's
wrappers before it (the plane proxy draw 0x827EE190: its stencil restore at +10C is after its draw at +E0) is not counted.
Limits [L]: a setter that reaches the recorder by a vcall of its own (not through the wrapper) is not in the call edges (and q
vcall does not list the wrappers' own recorder sites, so such a bypass cannot be searched); a kind whose draw is reached from
inside a setter's set section by a vcall (a UI sprite under 0x83346AC0's menu child draw, alpha off) is not seen by the window
check; a path a setter's ARMS cut (below, each with its reason) is not read.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

THREAD_OBJ = 0x82BA7238            # get_thread_block_word_1036_else_1040: the recorder object of the calling thread
INIT_FN = 0x82C22F88               # the recorder's initialiser (from its constructor sub_82C244A0): the cache words' start values
STACK = 'r1'
ARG = ('ARG',)                     # the first argument as the function received it (r3 at entry)
MAX_LITS = 8

# state -> its wrappers (recorder slot wrapper -> the cache word that slot keeps), its getters (wrappers that return a cache word:
# their word is READ from the listing), the device value from the words, and the method it rests on.
STATES = {
    'cmask': {'wrappers': {0x82BB7970: 92, 0x82BB79B8: 96, 0x82BB7A00: 100, 0x82BB7A48: 104, 0x82BB7A90: 108},
              'getters': (0x82BB82C8, 0x82BB81B0),
              'device': lambda w: 0xF if all(w.get(k) == 1 for k in (92, 96, 100, 104, 108)) else None,
              'method': 'the mask methods 0x82C23910 / 0x82C23980..0x82C23B60 record render state 53 = 15 when +92..+108 are 1'},
    'stencil': {'wrappers': {0x82BB76C0: 28},
                'getters': (0x82BB8228,),
                'device': lambda w: w.get(28),
                'method': 'the stencil method 0x82C23490 records render state 27 = the word'},
}
# A restorer -> the saver it pairs with: the restorer sets a word from its argument struct (+N), the saver stored the
# recorder getter's value of that word at +N of ITS argument struct. Checked here from both listings (the saver's store, the
# restorer's load); that the caller hands both the same struct is the reason, read at the caller's listing.
PAIRS = {0x82C1FB60: {'saver': 0x82C1FA98,
                      'why': 'both called by prelight_object_bake_iterations 0x82C20BC8 (+58 the saver, +174 the restorer) with r3 = sp+80'}}
# A setter's branch read one way (as a kind window's 'arms'), each with its reason.
ARMS = {0x82BEF748: {0x114: ('fall', "pass_zpass_draw +10C..+114: the early return of 0 when the model's word +220 is 0, AFTER the mask-off at +FC; "
                                      "+220 is the model's data (model_create_from_data 0x82BDB460 +3C stores its data argument there and "
                                      "returns at +40 when it is 0). +220 = 0 is an ordinary model state, not a failure: the same build "
                                      "clears it at +1C88 whenever +556 != 0 (the ctor 0x82BD0230 +B8 starts +556 at -1), and the release "
                                      "slot 45 0x82BD91B0 clears it. The prepass is reached only through model slot 41 0x82BD5C20 +44 "
                                      "(gated by the draw thread's product table 0x83929A40); which models reach it is not read: a model "
                                      "the z prepass draws has +220 set [L]. Taken, +92 would stay 0 until a setter writes 1 "
                                      "(0x825CECE0 +4C8): no colour from the prepass to there")}}


def _ops(x):
    return [o.strip() for o in x.ops.split(',')] if x.ops else []


def _mem(op):
    """'N(rB)' -> (N, 'rB') else None."""
    m = re.fullmatch(r'(-?(?:0x[0-9a-fA-F]+|\d+))\((r\d+)\)', op or '')
    return (int(m.group(1), 0), m.group(2)) if m else None


def _int(s):
    try:
        return int(s, 0) & 0xFFFFFFFF
    except (TypeError, ValueError):
        return None


def getter_word(insns):
    """-> W of a getter `lwz r3,0(r3); bl THREAD_OBJ; lwz r3,W(r3); ... blr`, else None."""
    seen = False
    for x in insns:
        ops = _ops(x)
        if x.mn == 'bl' and ops and _int(ops[0]) == THREAD_OBJ:
            seen = True
        elif seen and x.mn == 'lwz' and ops and ops[0] == 'r3':
            m = _mem(ops[1]) if len(ops) > 1 else None
            return m[0] if m and m[1] == 'r3' else None
        elif seen and x.mn in ('bl', 'bctrl'):
            return None
    return None


def init_words(insns):
    """-> {word: value} of the initialiser's `li rX,v` .. `stw rX,W(r3)` stores (straight-line code, first store of a word)."""
    regs, out = {}, {}
    for x in insns:
        ops = _ops(x)
        if x.mn == 'li' and len(ops) == 2:
            regs[ops[0]] = _int(ops[1])
        elif x.mn == 'stw' and len(ops) == 2:
            m = _mem(ops[1])
            if m and m[1] == 'r3' and regs.get(ops[0]) is not None:
                out.setdefault(m[0], regs[ops[0]])
        elif ops and re.fullmatch(r'r\d+', ops[0]) and not x.mn.startswith(('st', 'cmp', 'b')):
            regs.pop(ops[0], None)
    return out


def _step(x, st, fn, setters, getters):
    """The path state after x: registers / stack slots ('M', N) / argument words ('A', N) as frozensets of values (an int,
    ('G', word) = a getter's result, ARG = the first argument, ('AW', N) = the word loaded from argument +N);
    ('S', word) = frozenset of (value, offset) the last wrapper call of that word set on this path (value None = VAR)."""
    import qrstdiff as R
    st = dict(st)
    ops = _ops(x)
    mn = x.mn
    if mn in ('bl', 'bctrl', 'blrl') or (mn == 'b' and ops and _int(ops[0]) in setters):
        tgt = _int(ops[0]) if ops and mn in ('bl', 'b') else None
        if tgt in setters:
            v = st.get('r4')
            st[('S', setters[tgt])] = frozenset((k, x.addr - fn) for k in v) if v else frozenset({(None, x.addr - fn)})
        for r in R.VOLATILE:
            st.pop(r, None)
        if tgt in getters and ('S', getters[tgt]) not in st:   # after a set of the word on this path the getter returns that set value: no save token
            st['r3'] = frozenset({('G', getters[tgt])})
        return st
    if mn in ('stw', 'stwu') and len(ops) == 2:
        m = _mem(ops[1])
        if m and mn == 'stw' and (m[1] == STACK or st.get(m[1]) == frozenset({ARG})):
            key = ('M', m[0]) if m[1] == STACK else ('A', m[0])
            if ops[0] in st:
                st[key] = st[ops[0]]
            else:
                st.pop(key, None)
        return st
    if mn == 'lwz' and len(ops) == 2:
        m = _mem(ops[1])
        if m and m[1] == STACK and ('M', m[0]) in st:
            st[ops[0]] = st[('M', m[0])]
        elif m and m[1] != STACK and st.get(m[1]) == frozenset({ARG}):
            st[ops[0]] = st[('A', m[0])] if ('A', m[0]) in st else frozenset({('AW', m[0])})
        else:
            st.pop(ops[0], None)
        return st
    if mn == 'li' and len(ops) == 2:
        v = _int(ops[1])
        if v is None:
            st.pop(ops[0], None)
        else:
            st[ops[0]] = frozenset({v})
        return st
    if mn == 'mr' and len(ops) == 2:
        if ops[1] in st:
            st[ops[0]] = st[ops[1]]
        else:
            st.pop(ops[0], None)
        return st
    if mn in ('addi', 'ori') and len(ops) == 3 and ops[0] != STACK:
        b, k = st.get(ops[1]), _int(ops[2])
        if b and k is not None and all(isinstance(v, int) for v in b):
            st[ops[0]] = frozenset((((v + k) if mn == 'addi' else (v | k)) & 0xFFFFFFFF) for v in b)
        else:
            st.pop(ops[0], None)
        return st
    if ops and re.fullmatch(r'r\d+', ops[0]) and not mn.startswith(('st', 'cmp', 'b')):
        st.pop(ops[0], None)
    return st


def _join(a, b):
    if a is None:
        return dict(b)
    out = {}
    for k in set(a) | set(b):
        if isinstance(k, tuple) and k[0] == 'S':
            out[k] = a.get(k, frozenset({('INH', None)})) | b.get(k, frozenset({('INH', None)}))
        elif k in a and k in b:
            u = a[k] | b[k]
            if len(u) <= MAX_LITS:
                out[k] = u
    return out


def _is_return(x):
    mn = x.mn.rstrip('+-')
    return mn == 'blr' or (mn.startswith('b') and mn.endswith('lr') and not mn.startswith('bl'))


def exits(insns, fn, setters, getters, arms=None):
    """-> [(offset, path state at the exit)]: every return (blr and its conditional forms) and every tail branch out of the
    function (`b` to the register-restore helper or another function). Code no path from the entry reaches (a jump table's arms
    after a `bctr`) starts from the slot state of the last `bctr` before it, so a set made before a switch is never lost.
    arms = {offset: 'taken' | 'fall'}: a branch read one way."""
    import qrstdiff as R
    arms = arms or {}
    idx = {x.addr: i for i, x in enumerate(insns)}
    n = len(insns)
    end = insns[-1].addr + 4 if insns else fn
    entered = {0}
    for i, x in enumerate(insns):
        if x.mn.rstrip('+-') not in R.NO_FALL:
            entered.add(i + 1)
        t = R._target(x)
        if t is not None and t in idx:
            entered.add(idx[t])
    state = [None] * n
    pending, seed, last_bctr = set(), 0, None
    while True:
        while seed < n and (state[seed] is not None or (seed in entered and seed != 0)):
            seed += 1
        if seed >= n:
            break
        state[seed] = ({k: v for k, v in last_bctr.items() if isinstance(k, tuple) and k[0] == 'S'} if seed and last_bctr
                       else {'r3': frozenset({ARG})} if seed == 0 else {})
        pending.add(seed)
        while pending:
            i = min(pending)
            pending.discard(i)
            x = insns[i]
            out = _step(x, state[i], fn, setters, getters)
            mn = x.mn.rstrip('+-')
            if mn == 'bctr':
                last_bctr = _join(last_bctr, out)
            arm = arms.get(x.addr - fn)
            succ = []
            if mn not in R.NO_FALL and i + 1 < n and arm != 'taken':
                succ.append(i + 1)
            t = R._target(x)
            if t is not None and t in idx and arm != 'fall':
                succ.append(idx[t])
            for j in succ:
                m = _join(state[j], out)
                if m != state[j]:
                    state[j] = m
                    pending.add(j)
    out = []
    for i, x in enumerate(insns):
        if state[i] is None:
            continue
        mn = x.mn.rstrip('+-')
        t = R._target(x) if mn == 'b' else None
        if _is_return(x):
            out.append((x.addr - fn, state[i]))
        elif mn == 'b' and t is not None and not (fn <= t < end):
            out.append((x.addr - fn, _step(x, state[i], fn, setters, getters)))
        elif i == n - 1 and mn not in R.NO_FALL:
            out.append((x.addr - fn + 4, _step(x, state[i], fn, setters, getters)))
    return out


def _saved_by(saver, insns_of, setters, getters):
    """-> {N: word} of the saver's argument words that hold a getter's result on EVERY exit."""
    outs = exits(insns_of(saver), saver, setters, getters, ARMS.get(saver))
    if not outs:
        return {}
    keys = None
    for _off, st in outs:
        got = {k[1]: next(iter(v))[1] for k, v in st.items()
               if isinstance(k, tuple) and k[0] == 'A' and len(v) == 1 and isinstance(next(iter(v)), tuple) and next(iter(v))[0] == 'G'}
        keys = got if keys is None else {n: w for n, w in keys.items() if got.get(n) == w}
    return keys or {}


def census(name, insns_of, callers_of):
    """-> {'value': device value or None, 'setters': [fn], 'leaks': [text], 'start': {word: v}, 'problems': [text],
    'pairs': [text], 'arms': [text]}."""
    import qrstdiff as R
    spec = STATES[name]
    res = {'value': None, 'setters': [], 'leaks': [], 'start': {}, 'problems': [], 'pairs': [], 'arms': []}
    getters = {}
    for g in spec['getters']:
        w = getter_word(insns_of(g))
        if w is None:
            res['problems'].append(f'{R.hx(g)}: not a recorder getter (no `lwz r3,W(r3)` after the thread-object call)')
        else:
            getters[g] = w
    words = set(spec['wrappers'].values())
    start = {w: v for w, v in init_words(insns_of(INIT_FN)).items() if w in words}
    res['start'] = start
    if set(start) != words:
        res['problems'].append(f'{R.hx(INIT_FN)}: start value not read for words {sorted(words - set(start))}')
        return res
    fns = callers_of(sorted(spec['wrappers']))
    res['setters'] = fns
    for f in fns:
        pair = PAIRS.get(f)
        saved = _saved_by(pair['saver'], insns_of, spec['wrappers'], getters) if pair else {}
        for a_off, (how, why) in sorted(ARMS.get(f, {}).items()):
            res['arms'].append(f'{R.hx(f)} +{a_off:X} read {how}: {why}')
        for off, st in exits(insns_of(f), f, spec['wrappers'], getters, {k: v[0] for k, v in ARMS.get(f, {}).items()}):
            for w in sorted(words):
                for v, so in sorted(st.get(('S', w), frozenset({('INH', None)})), key=lambda vo: (str(vo[0]), vo[1] or 0)):
                    if v == 'INH' or v == start[w] or v == ('G', w):
                        continue
                    if isinstance(v, tuple) and v[0] == 'AW' and saved.get(v[1]) == w:
                        res['pairs'].append(f"{R.hx(f)} +{so:X} restores +{w} from its argument +{v[1]}, saved there by "
                                            f"{R.hx(pair['saver'])} ({pair['why']})")
                        continue
                    what = ('VAR' if v is None else f'the getter of +{v[1]}' if isinstance(v, tuple) and v[0] == 'G'
                            else f'argument word +{v[1]}' if isinstance(v, tuple) else str(v))
                    res['leaks'].append(f'{R.hx(f)} returns at +{off:X} with +{w} = {what} (set at +{so:X})')
    if not res['leaks'] and not res['problems']:
        res['value'] = spec['device'](start)
    return res


def callers_reader(con):
    def callers_of(addrs):
        q = ','.join('?' * len(addrs))
        return sorted({r['src'] for r in con.execute(f'SELECT DISTINCT src FROM edges WHERE dst IN ({q})', tuple(addrs)).fetchall()})
    return callers_of


def fill(st, insns_of, fns=()):
    """Fill every field of STATES that no window of the kind set (st[field]['unset']) from its census; -> problems.
    insns_of must carry .callers_of (q's call edges): without it nothing is filled (the field stays UNSETTLED).
    fns = the functions of the kind's game windows, each a function or (function, window end): one of them a setter of the state
    means the draw runs inside that setter's set section (the start value is not its state): the field is not filled, the setter
    is named (UNSETTLED). With a window end, only a setter that calls a wrapper of the state before that end counts (S111
    RSTCOVER-A review: a setter that only restores after its draw leaves the draw the inherited value).
    The census is cached on insns_of (.inherit_cache) when it has the attribute."""
    import qrstdiff as R
    callers_of = getattr(insns_of, 'callers_of', None)
    if callers_of is None:
        return []
    cache = getattr(insns_of, 'inherit_cache', None)
    if cache is None:
        cache = {}
    problems = []
    for name in STATES:
        g = st.get(name)
        if not g or not g['unset']:
            continue
        if name not in cache:
            cache[name] = census(name, insns_of, callers_of)
        res = cache[name]
        problems += [f'inherited {name}: {p}' for p in res['problems']]
        n = len(res['setters'])
        inside = set()
        for item in fns:
            fn, hi = item if isinstance(item, tuple) else (item, None)
            if fn in res['setters'] and (hi is None or any(off < hi for off, _t, _v in R.calls(insns_of(fn), fn, STATES[name]['wrappers']))):
                inside.add(fn)
        inside = sorted(inside)
        if inside:
            g['unset'] = False
            g['inh'] = [f"not inherited: the kind's window function {', '.join(R.hx(f) for f in inside)} is itself a setter of it "
                        f"(the draw runs inside its set section; the words it sets there are not all read by the windows)"]
        elif res['value'] is not None:
            g['unset'] = False
            g['vals'].setdefault(res['value'], []).append(
                f'inherited: all {n} setters leave it as found ({len(res["pairs"])} by a save / restore pair, {len(res["arms"])} arm read one way; '
                f'q call edges; start 0x{INIT_FN:08X})')
        elif res['leaks']:
            g['unset'] = False
            g['inh'] = [f'{len(res["leaks"])} setter exits leave another value: ' + '; '.join(res['leaks'][:4])
                        + (f'; ... {len(res["leaks"]) - 4} more' if len(res['leaks']) > 4 else '')]
    return problems
