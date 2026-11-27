r"""q pairs' RELATION column (S111 PAIRMEAN): what a pair row means, MIRRORS / WATCHES / CITES.

A `q pairs` row says our code MENTIONS a game function; only a MIRRORS row says our function REIMPLEMENTS it (a port: the diff tools
compare its facts with the game function's). q codediff, q fieldsdiff and q pairs --coverage pair only MIRRORS rows (plus their own
mirror-by-construction sources: kTable addr cells, rstdiff KINDS, fieldsdiff records); WATCHES / CITES rows stay listed in q pairs.

Code shape first, comment words second, never a guess (an undecidable row is CITES). Per row of our function F citing game function X:
  1. WATCHES  code          the row's token is in CODE (a call, a hook head, an address our code uses: a guest data read, a registration)
  2. WATCHES  hook          F calls or hooks the game: a `sub_8XXXXXXX` token in F's code (REX_HOOK_RAW(sub_X) head, call(sub_X), sub_X(ctx,
                            base), __imp__sub_X), an `Original(` call, or a guest-entry parameter (PPCContext / PPCRegister: a REX hook body, a
                            mid-asm hook): a hook / driver / wrapper's every citation is about the game it drives
  3. WATCHES  handler       F is called from a hook-shaped function H (rule 2) whose own rows cite X (H = REX_HOOK_RAW(sub_X) { ... F(...) }, a
                            mid-asm hook naming X): F is that hook's body (one call down; the table `hooks` keeps each H's callees)
  4. words of the citation's comment block (the comment lines around it; a claim's / seam row's own text):
       WATCHES words only   WATCHES (hook, probe, logs, watches, trace, census, wraps, after X returns / runs, the original)
       both kinds of words  CITES (undecidable)
  5. (no logger shape: a log call is not a role; FrontEnd::DrawInstanced, a port, logs its census: a logger is told by its words only)
  6. CITES    reader        F reads the live game (GUEST: the guest memory base passed on, a guest accessor, a guest address in code, an
                            injected guest reader): an input reader's relation to X is not stated by its shape (MIRRORS words there are
                            CITES too: the rule they name is often a helper's, RINGRECOMPUTE's `SkirtDrawnMask` line)
  7. MIRRORS  words         MIRRORS words only (ports, reimplements, as the game does, the game's own rule / test, the one rule)
  8. MIRRORS  computes      F never touches the game (rules 2, 6 absent) and computes a value (a return value / an assignment): a function
                            that cites a game function it never calls, hooks or reads can only be reproducing it
  9. CITES    (the rest: a function that computes nothing, a type / constant / file-scope comment with no words)
A row that is not a function's (a type, a constant, file scope): rule 1, then words (rule 4 / 7), else CITES.
"""
import json
import os
import re

# selftest only: WWE_QPAIRS_PLANT=rel_all_mirrors calls every row MIRRORS (the old pairing: every precise row a mirror)
PLANT = os.environ.get('WWE_QPAIRS_PLANT', '')
RELS = ('MIRRORS', 'WATCHES', 'CITES')
VERSION = '1'   # bump on any rule / word-list change: q pairs' cache keys its rows to it (qpairs.update rescans every file)

_GAME_FN = re.compile(r'(?<![0-9A-Za-z])sub_8[0-9A-Fa-f]{7}(?![\w])')
_ORIGINAL = re.compile(r'\bOriginal\s*\(')
_ENTRY = re.compile(r'\bPPCContext\b|\bPPCRegister\b')
# rule 6: the ways our code reads the live game (each named in the reason)
GUEST = (
    (re.compile(r'(?:\(|,)\s*base\s*[,)]|\buint8_t\s*\*\s*base\b'), 'the guest memory base `base`'),
    (re.compile(r'\bGuest\w*\s*\(|\bREX_KERNEL_MEMORY\b|\bPPC_(?:LOAD|STORE)_\w+'), 'a guest accessor'),
    (re.compile(r'(?<![\w])0x8[23][0-9A-Fa-f]{6}(?![\w])'), 'a guest address in code'),
    (re.compile(r'\bread_word\b|\bread_\b|\bReadBase\b|\bReadGuest\b|\bReadWord\s*\(\s*(?:read_?|rw|&ReadBase)\b'), 'an injected guest reader'),
    (re.compile(r'\bmem\s*\.\s*(?:u8|u16|u32|s8|s16|s32|f32|words|bytes)\s*\('), 'the guest memory view `mem`'),
    (re.compile(r'\be\s*\.\s*(?:U8|U16|U32|F32|Put8|Put16|Put32|Call)\s*\('), 'the guest engine `e`'),
)
_COMPUTES = re.compile(r'\breturn\s+[^;\s]|[^=!<>+\-*/%&|^~]=[^=]|[+\-*/%&|^]=|<<=|>>=|\+\+|--')
WATCH_WORDS = re.compile(r"\bhook(?:s|ed|ing)?\b|\bdetour|\bprobe[sd]?\b|\blog(?:s|ged|ging)?\b|\bwatch(?:es|ed|ing)?\b|\btrace[sd]?\b"
                         r"|\bcensus\b|\bwraps?\b|\bthe original\b|\bbefore the original\b|\bafter\b[^.;]{0,40}?\b(?:return(?:s|ed)?|ran|runs)\b", re.I)
MIRROR_WORDS = re.compile(r"\bports?\b|\bported\b|\breimplement\w*|\bas (?:the game|sub_8[0-9a-f]{7}|it) does\b"
                          r"|\b(?:the game'?s|its|the draw'?s) (?:own )?(?:rule|test|formula|order)\b|\bsame (?:rule|test|formula|order) as\b"
                          r"|\bmirrors (?:the game|sub_8)|\bthe one rule\b", re.I)
_TAG = re.compile(r'\[[CLUP](?:[ :][^\]]*)?\]')
_CALLEE = re.compile(r'((?:\w+::)*)(\w+)\s*(?:<[^<>();{}]*>)?\s*\(')


def _is_comment_only(raw, masked):
    return raw.strip() != '' and masked.strip() == '' and raw.lstrip().startswith(('//', '/*', '*'))


def comment_block(lines, mlines, ln):
    """the comment text a citation on line ln sits in: a comment-only line with its neighbours (up to 12 each way); a code line's
    trailing comment with the comment-only lines right below it"""
    if ln < 1 or ln > len(lines):
        return ''
    ml = lambda i: mlines[i - 1] if i - 1 < len(mlines) else ''
    raw = lines[ln - 1]
    if _is_comment_only(raw, ml(ln)):
        a = ln
        while a > 1 and ln - a < 12 and _is_comment_only(lines[a - 2], ml(a - 1)):
            a -= 1
        b = ln
        while b < len(lines) and b - ln < 12 and _is_comment_only(lines[b], ml(b + 1)):
            b += 1
        return ' '.join(lines[i - 1].strip() for i in range(a, b + 1))
    m = ml(ln)
    cut = len(m.rstrip())
    tail = raw[cut:] if '//' in raw[cut:] or '/*' in raw[cut:] else ''
    b = ln
    while tail and b < len(lines) and b - ln < 4 and _is_comment_only(lines[b], ml(b + 1)):
        b += 1
    return ' '.join([tail.strip()] + [lines[i - 1].strip() for i in range(ln + 1, b + 1)])


def words(text):
    """-> ('W' | 'M' | 'MW' | '', the words found); an evidence tag ([C probe], [L listing]) names how a fact was got, not our role: left out"""
    text = _TAG.sub(' ', text or '')
    w = WATCH_WORDS.findall(text)
    m = MIRROR_WORDS.findall(text)
    return ('M' if m else '') + ('W' if w else ''), [x.strip() for x in (m + w)][:3]


class Facts:
    """the code shape of OUR functions in one file (qsym.Src spans over the masked text): per qualified name"""

    def __init__(self, ow):
        self.ow = ow
        self.src = ow.src
        self._f = {}

    def of(self, qual):
        if qual in self._f:
            return self._f[qual]
        spans = [sp for sp in self.ow.fn_list if (sp['qual'] or sp['name']) == qual and sp['end'] is not None]
        ml = self.ow.ml
        body = '\n'.join(ml[k - 1] for sp in spans for k in range(sp['start'], min(sp['end'], len(ml)) + 1))
        inner = '\n'.join(ml[k - 1] for sp in spans for k in range((sp['open'] or sp['start']), min(sp['end'], len(ml)) + 1))
        f = {'hook': '', 'reader': '', 'computes': bool(_COMPUTES.search(inner.split('{', 1)[-1])),
             'games': set(), 'callees': []}
        g = _GAME_FN.search(body)
        if g:
            ln = spans[0]['start'] + body[:g.start()].count('\n') if spans else 0
            f['hook'] = 'calls / hooks %s in its code (line %d)' % (g.group(0), ln)
        elif _ORIGINAL.search(body):
            f['hook'] = 'calls Original(): a wrapper of the game function'
        elif _ENTRY.search(body):
            f['hook'] = 'a guest entry (PPCContext / PPCRegister parameter): a hook body'
        for rx, why in GUEST:
            if rx.search(body):
                f['reader'] = why
                break
        if f['hook']:
            f['games'] = {t[4:].upper() for t in _GAME_FN.findall(body)}
            seen = set()
            for m in _CALLEE.finditer(inner):
                k = (m.group(1).rstrip(':'), m.group(2))
                if k not in seen:
                    seen.add(k)
                    f['callees'].append(k)
        self._f[qual] = f
        return f


def classify(row, facts, text):
    """-> (rel, why) of one row before the handler pass (rule 3); facts = Facts.of(row's function) or None; text = its words"""
    if PLANT == 'rel_all_mirrors':
        return 'MIRRORS', 'planted: every row'
    if row['src'] == 'code':
        return 'WATCHES', 'code: the address is used in our code'
    wk, wl = words(text)
    if row['kind'] != 'func' or facts is None:
        if wk == 'W':
            return 'WATCHES', 'words: ' + ', '.join(wl)
        if wk == 'M':
            return 'MIRRORS', 'words: ' + ', '.join(wl)
        return 'CITES', ('words of both kinds: ' + ', '.join(wl)) if wk else 'no shape (not a function) and no words'
    if facts['hook']:
        return 'WATCHES', 'hook: ' + facts['hook']
    if wk == 'W':
        return 'WATCHES', 'words: ' + ', '.join(wl)
    if wk == 'MW':
        return 'CITES', 'words of both kinds: ' + ', '.join(wl)
    if facts['reader']:
        return 'CITES', 'reader: reads the live game (%s)%s' % (facts['reader'], '; its port words (%s) may name a helper' % ', '.join(wl) if wk else '')
    if wk == 'M':
        return 'MIRRORS', 'words: ' + ', '.join(wl)
    if facts['computes']:
        return 'MIRRORS', 'computes: a value, and never calls, hooks or reads the game'
    return 'CITES', 'no shape: computes nothing, and no words'


def hook_rows(facts, rows):
    """[(fn, games json, callees json)] of the hook-shaped functions of one file (rule 3's callers): games = the code tokens plus every
    address the function's own rows cite"""
    out = []
    by = {}
    for r in rows:
        if r['kind'] == 'func':
            by.setdefault(r['ours'], set()).add(r['game'])
    for qual, f in facts._f.items():
        if f['hook'] and f['callees']:
            g = sorted(f['games'] | by.get(qual, set()))
            out.append((qual, json.dumps(g), json.dumps(f['callees'])))
    return out


def finish(con):
    """rule 3 over the whole table: rel = rel0, except a function row whose (function, game) a hook-shaped caller names -> WATCHES"""
    idx = {}
    for fn, games, callees in con.execute('SELECT fn, games, callees FROM hooks'):
        gs = set(json.loads(games))
        for q, name in json.loads(callees):
            idx.setdefault(name, []).append((q, gs, fn))
    upd = []
    for rid, ours, kind, game, rel0, why0 in con.execute('SELECT rowid, ours, kind, game, rel0, why0 FROM pairs'):
        rel, why = rel0, why0
        if kind == 'func' and PLANT != 'rel_all_mirrors' and not why0.startswith(('hook:', 'code:')):
            parts = [p for p in re.sub(r'\(.*$', '', ours).split('::') if p]
            name = parts[-1] if parts else ''
            for q, gs, fn in idx.get(name, ()):
                if fn == ours or game not in gs:
                    continue
                qs = [p for p in q.split('::') if p]
                if qs and parts[-1 - len(qs):-1] != qs:
                    continue
                rel, why = 'WATCHES', 'handler: called from the hook %s, which names sub_%s' % (fn, game)
                break
        upd.append((rel, why, rid))
    con.executemany('UPDATE pairs SET rel = ?, why = ? WHERE rowid = ?', upd)
