r"""q draworder, the readers added by S111 DRAWORDER2 (0 game runs). Pure helpers: this file imports nothing of qdraworder.

  * parse_see_rank: SeeRank's body (see_through_order.h) read line by line. A rank line is `if (COND) return N;`; COND is `&&` terms: `k.FIELD`,
    `k.FIELD == N`, `Helper(k)` (the helper's own `return COND;` is read the same way: LateAdditive, Rekeyed), `!g_plant_*` (the planted twin's
    off state: dropped); a positive `g_plant_*` term makes the line the planted twin's arm (skipped, as FrontEnd::Record's). `return k.F ? A : B;`
    is the else. A line that is none of these is a PROBLEM, never a silent skip (before S111 DRAWORDER2 a line not starting `if (k.` was skipped).
    S111 REKEYORDER2: a `(A || B)` group (each arm read as a COND; the line gives one rank entry per arm) and the place term
    `k.place >= NAME`, NAME the first place of walked list 1 (`= int32_t(...::kMaxNodes)`): the key PLACE_LATE, "placed in a walked list after
    list 0 (33)", which a family takes when all its categories are at or after char_draw_list.h kDrawLists[1] (late_list_cat).
  * place_ranks: the ranks SeeThroughBefore orders by place (`if ((ra == 1 || ra == kRankRekeyed) && ...) return a.place < b.place;`).
  * game_rekey: the entrance scene's categories, read from the re-key row of render_categories.md's "The re-key moves" table.
  * late_bodies: whether our entrance moves a re-keyed body into the blended list (Record calls draw_place::TakeLateBodies and StampItems gives
    the moved item place_block), read from front_end.cpp / draw_place_rows.h.
"""
import re

REKEY_ROW = 'entrance re-key'
PLACE_LATE = 'place_late'   # SeeRank's `k.place >= kPlaceAfterLateNodes`: the item's walked-list place is in list 1 (41) or after (S111 REKEYORDER2)


def _split_top(cond, op):
    """cond split at `op` ('&&' / '||') outside parentheses"""
    out, depth, cur, i = [], 0, '', 0
    while i < len(cond):
        c = cond[i]
        if depth == 0 and cond.startswith(op, i):
            out.append(cur.strip())
            cur, i = '', i + len(op)
            continue
        depth += (c == '(') - (c == ')')
        cur += c
        i += 1
    out.append(cur.strip())
    return out


def _unwrap(t):
    """a term wrapped whole in one pair of parentheses -> its inside; any other term unchanged"""
    if not (t.startswith('(') and t.endswith(')')):
        return t
    depth = 0
    for i, c in enumerate(t):
        depth += (c == '(') - (c == ')')
        if depth == 0 and i < len(t) - 1:
            return t   # `(a) && (b)`: the first pair closes before the end
    return t[1:-1].strip()


def _terms(cond):
    return [_unwrap(t) for t in _split_top(cond, '&&')]


def _place_threshold(name, text):
    """-> problem | None: NAME must be the first place of walked list 1 (char_draw_list.h: list k's word i = k * kMaxNodes + i)"""
    if re.search(r'inline constexpr \w+ ' + re.escape(name) + r'\s*=\s*(?:int32_t\()?(?:[\w:]*::)?kMaxNodes\)?;', text):
        return None
    return f'the place threshold {name} is not read as the first place of walked list 1 (`inline constexpr int32_t {name} = int32_t(...::kMaxNodes);`)'


def _cond(cond, text, depth=0):
    """-> (alts | None, plant bool, problem str | None). alts = the condition's alternatives, each a preds list (an `||` group gives one per
    arm; no group = one); None = unreadable"""
    alts, plant = [[]], False
    for t in _terms(cond):
        arms = _split_top(t, '||')
        if len(arms) > 1:   # S111 REKEYORDER2: `(k.place_block || k.character)`
            sub = []
            for a in arms:
                sa, sp, prob = _cond(a, text, depth + 1)
                if sa is None:
                    return None, False, prob
                if sp:
                    return None, False, f'the term `{t}`: a planted twin inside an `||` group is not read'
                sub += sa
            alts = [x + y for x in alts for y in sub]
            continue
        m = re.fullmatch(r'k\.(\w+)(?:\s*==\s*(\d+))?', t)
        if m:
            alts = [x + [m.group(1) + (m.group(2) or '')] for x in alts]
            continue
        p = re.fullmatch(r'k\.place\s*>=\s*(\w+)', t)
        if p:   # S111 REKEYORDER2: Rekeyed's place term
            prob = _place_threshold(p.group(1), text)
            if prob:
                return None, False, prob
            alts = [x + [PLACE_LATE] for x in alts]
            continue
        if re.fullmatch(r'!\s*g_plant\w*(?:\.load\([^)]*\))?', t):
            continue
        if re.fullmatch(r'g_plant\w*(?:\.load\([^)]*\))?', t):
            plant = True
            continue
        h = re.fullmatch(r'(\w+)\(k\)', t)
        if h and depth == 0:
            hb = re.search(r'inline bool ' + h.group(1) + r'\(const SeeKey& k\)\s*\{\s*return ([^;]*);\s*\}', text)
            if not hb:
                return None, False, f'the helper {h.group(1)}(k) is not read (no `inline bool {h.group(1)}(const SeeKey& k) {{ return COND; }}`)'
            sub, sp, prob = _cond(hb.group(1), text, 1)
            if sub is None:
                return None, False, prob
            alts = [x + y for x in alts for y in sub]
            plant = plant or sp
            continue
        return None, False, f'the term `{t}` is not read'
    return alts, plant, None


def parse_see_rank(see_code, plant=None):
    """see_code = see_through_order.h with // comments cut. -> (ranks [(pred 'a+b', rank)], rank_else (pred, a, b) | None, problems).
    plant 'skip_unread' = the parser before S111 DRAWORDER2 (only lines starting `if (k.` are read; the rest skipped silently): tests only."""
    probs = []
    m = re.search(r'inline int SeeRank\(', see_code)
    if not m:
        return [], None, [f'SeeRank not found']
    lines = see_code[m.start():].split('\n')
    end = next((i for i, ln in enumerate(lines) if i and ln.startswith('}')), len(lines))
    ranks, else_ = [], None
    for ln in lines[1:end]:
        s = ln.strip()
        if not s:
            continue
        if plant == 'skip_unread':
            mm = re.match(r'if \(k\.(\w+)(?:\s*==\s*(\d+))?.*?\)\s*return (\d+);', s)
            if mm:
                ranks.append((mm.group(1) + (mm.group(2) or ''), int(mm.group(3))))
            e = re.match(r'return k\.(\w+) \? (\d+) : (\d+);', s)
            if e:
                else_ = (e.group(1), int(e.group(2)), int(e.group(3)))
            continue
        e = re.fullmatch(r'return k\.(\w+) \? (\d+) : (\d+);', s)
        if e:
            else_ = (e.group(1), int(e.group(2)), int(e.group(3)))
            continue
        r = re.fullmatch(r'if \((.*)\)\s*return (\w+)(\(k\))?;', s)
        if not r:
            probs.append(f'SeeRank: the line `{s}` is not a rank line (if (COND) return N; / return k.F ? A : B;)')
            continue
        alts, is_plant, prob = _cond(r.group(1), see_code)
        if alts is None:
            probs.append(f'SeeRank: the line `{s}`: {prob}')
            continue
        if is_plant:
            continue   # the planted twin's arm
        if not r.group(2).isdigit() or r.group(3):
            probs.append(f'SeeRank: the line `{s}` returns something that is not a rank number')
            continue
        if any(not a for a in alts):
            probs.append(f'SeeRank: the line `{s}` tests no key field')
            continue
        ranks += [('+'.join(a), int(r.group(2))) for a in alts]   # an `||` group: one entry per arm, in the line's order
    if else_ is None and plant != 'skip_unread':
        probs.append('SeeRank: the else line `return k.F ? A : B;` not read')
    return ranks, else_, probs


def place_ranks(before_body, see_code):
    """-> (set of the ranks SeeThroughBefore orders by place, problems). The line `if (RANKS && ...) return a.place < b.place;`, RANKS =
    `ra == N` or an `||` group of them, N a number or a name of `inline constexpr int NAME = N;` in see_code (S111 REKEYORDER2: kRankRekeyed).
    No such line = no rank is ordered by place (the rule before S111 PROPORDER); a line whose rank term is not read is a problem."""
    out, probs = set(), []
    for ln in (before_body or '').split('\n'):
        s = ln.strip()
        if 'return a.place < b.place;' not in s:
            continue
        mm = re.fullmatch(r'if \((.*)\)\s*return a\.place < b\.place;', s)
        arms = next((a for a in (_split_top(t, '||') for t in (_terms(mm.group(1)) if mm else []))
                     if all(re.fullmatch(r'ra == \w+', x) for x in a)), None)
        if arms is None:
            probs.append(f'SeeThroughBefore: the place line `{s}` has no rank term read (ra == N, or an || group of them)')
            continue
        for a in arms:
            n = a.split('==')[1].strip()
            c = re.search(r'inline constexpr int ' + n + r'\s*=\s*(\d+);', see_code) if not n.isdigit() else None
            if not n.isdigit() and not c:
                probs.append(f'SeeThroughBefore: the place-ordered rank {n} is not read (no `inline constexpr int {n} = N;`)')
                continue
            out.add(int(n) if n.isdigit() else int(c.group(1)))
    return out, probs


def late_list_cat(lists_text):
    """-> (category | None, problems): the category of walked list 1 (char_draw_list.h kDrawLists[1], 41). A family whose categories are all at
    or after it has its place from kMaxNodes on in ours (list k's word i = k * kMaxNodes + i; a body after it: cat33_places.h BodyPlaceOf, 42 ->
    2 * kMaxNodes - 1), so it takes SeeRank's place term PLACE_LATE. Not read = no family takes it (a problem)."""
    m = re.search(r'kDrawLists\[\d+\]\s*=\s*\{([^}]*)\}', lists_text or '')
    items = [x.strip() for x in m.group(1).split(',')] if m else []
    if len(items) < 2 or not items[1].isdigit():
        return None, ['char_draw_list.h: kDrawLists (list 1 a number) not read: no family takes the place term of SeeRank']
    return int(items[1]), []


def rank_match(pred, preds):
    return set(pred.split('+')) <= set(preds)


def game_rekey(doc_text, fams):
    """-> ({fam name: [categories]}, problems): the categories the entrance re-key moves each family to, read from the re-key row's
    "What moves" cell by the family's `rekey` regex (its groups are the categories). A family that keeps part of its categories
    (rekey_keep) adds them to the old ones."""
    cell = None
    for line in doc_text.replace('\\|', '\x01').split('\n'):
        if line.startswith('|'):
            cells = [c.strip().replace('\x01', '|') for c in line.strip().strip('|').split('|')]
            if len(cells) >= 3 and cells[0].startswith(REKEY_ROW):
                cell = cells[2]
                break
    probs, out = [], {}
    if cell is None:
        return out, [f'the "{REKEY_ROW}" row of the re-key moves table is not in the doc']
    for f in fams:
        if not f.rekey:
            continue
        mm = re.search(f.rekey, cell)
        if not mm:
            probs.append(f'{f.name}: its re-key text /{f.rekey}/ is not in the re-key row')
            continue
        out[f.name] = sorted({int(g) for g in mm.groups() if g})
    return out, probs


def late_bodies(rec_body, place_rows_text):
    """-> (bool, problems): our entrance moves a re-keyed body's opaque meshes into the blended list at the character rank:
    FrontEnd::Record calls draw_place::TakeLateBodies(...) and StampItems marks a moved (late_body) item place_block"""
    probs = []
    call = bool(rec_body and re.search(r'draw_place::TakeLateBodies\(', rec_body))
    mark = bool(place_rows_text and re.search(r'place_block\s*=\s*p && \(p->block \|\| late\)', place_rows_text))
    if not call:
        probs.append('FrontEnd::Record: draw_place::TakeLateBodies call not found (a re-keyed body stays in the opaque list)')
    if not mark:
        probs.append('draw_place_rows.h: StampItems `place_block = p && (p->block || late)` not found (a moved body would not take the character rank)')
    return call and mark, probs


def gm_preds(gm_text):
    """-> (frozenset of SeeKey fields our gm category-36 rows take, problems): GmNodeObjectFlags ors kObjNodeLate into the object flags
    (a late node: SeeKey::late_node); without it the gm row keeps the material's state."""
    m = re.search(r'inline uint32_t GmNodeObjectFlags\([^)]*\)\s*\{(.*?)\n\}', gm_text or '', re.S)
    if not m:
        return None, ['gm_node_state.h: GmNodeObjectFlags not found']
    body = '\n'.join(ln.split('//', 1)[0] for ln in m.group(1).split('\n'))
    return (frozenset({'late_node'}) if re.search(r'\bkObjNodeLate\b', body) else frozenset()), []
