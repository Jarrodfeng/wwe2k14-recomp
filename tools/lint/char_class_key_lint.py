"""S111 SEECHAR review: an order key of FrontEnd::Record says "a character's" by the object's class, never by the material's effect.
  py -I tools\\lint\\char_class_key_lint.py [--root ROOT] [--verbose]

The game's character nodes choose what they draw by the character (the opaque node sub_825A3908, the translucent node sub_8259FAE8: a created character's
body layers and every character's own SUBM pieces), never by a material's effect (docs\\research\\s111\\FADECAW.md, docs\\research\\s111\\SEECHAR.md). So in
src\\renderer_scene\\front_end.cpp FrontEnd::Record an order / rank / turn key (SeeKey::character, SeeRank, SeeCharacter, body_order BodyObject / TurnOf /
PlaceOf, the StampItems / SortSeeThrough / OrderOpaque keys) takes "a character's" from the object's slot (piece_owner.h CharSlotOf, Item::char_slot).

Every IsCharEffect( call in FrontEnd::Record (code only: comments and string literals stripped; one statement = the text up to ; { or }) is sorted:
  - an ORDER use: its statement holds an order token, or it stores into a local / member name that a later statement with an order token reads (one hop:
    a local inside its own block, a member anywhere in Record). It must (a) sit behind a test plant read on its own statement (`g_plant_X ... && ...
    IsCharEffect(`: the effect only as a planted twin's input) and (b) hand the key through the class: SeeCharacter(<..char_slot..>, ...) or
    BodyObject(<name>, ...) whose <name> is declared from char_slot / CharSlotOf. Else FAIL.
  - any other use (a shader / variant / bind choice: the technique pick, the two-pass cull rule, the light-map rule): allowed, listed by --verbose.
The planted twins (run every time, in memory, inserted into the real Record): the pre-SEECHAR rank key, the pre-FADECAW turn key, a key through a local,
a key through a member, and a class key whose slot is not CharSlotOf's must each fail; a shader use must pass. Exit 0 pass, 1 a finding, 2 could not run."""
import pathlib
import re
import sys

FILE = 'src/renderer_scene/front_end.cpp'
RECORD = re.compile(r'\bbool\s+FrontEnd::Record\s*\(')
CALL = re.compile(r'\bIsCharEffect\s*\(')
ORDER = re.compile(r'\.character\b|\bSee(?:Rank|Character)\s*\(|\bBodyObject\s*\(|\bTurnOf\s*\(|\bPlaceOf\s*\(|\bOrderOpaque\s*\(|\bSortSeeThrough\s*\(|'
                   r'\bStampItems\s*\(|\.(?:place|rank|turn)\b|\b(?:rank|turn)\b')
PLANT = re.compile(r'\bg_plant_\w+')
LOCAL = re.compile(r'(?:\bconst\s+)?\b(?:bool|auto)\s+(\w+)\s*=(?!=)')
MEMBER = re.compile(r'\b\w+\.(\w+)\s*=(?!=)')
CLASS_DIRECT = re.compile(r'\bSeeCharacter\s*\(\s*[\w.>\-]*char_slot\b')
CLASS_NAMED = re.compile(r'\b(?:SeeCharacter|BodyObject)\s*\(\s*(\w+)\s*,')


def strip(text):
    """Comments, string and char literals blanked (newlines kept, so line numbers stay)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        elif c == 'R' and text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            k = text.find('(', i + 2)
            delim = text[i + 2:k] if k > 0 else ''
            j = text.find(')' + delim + '"', k + 1) if k > 0 else -1
            j = n if j < 0 else j + len(delim) + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(' ' * (j - i)); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


def record_body(code):
    """(start, end) offsets of FrontEnd::Record's body braces in the stripped text, or None."""
    m = RECORD.search(code)
    if not m:
        return None
    depth, i = 0, m.end() - 1
    while i < len(code):   # the parameter list
        if code[i] == '(':
            depth += 1
        elif code[i] == ')':
            depth -= 1
            if depth == 0:
                break
        i += 1
    b = code.find('{', i)
    if b < 0:
        return None
    depth = 0
    for j in range(b, len(code)):
        if code[j] == '{':
            depth += 1
        elif code[j] == '}':
            depth -= 1
            if depth == 0:
                return b, j
    return None


def statements(code, start, end):
    """[(line, depth, text)]: Record's body cut at ; { }. depth = the brace depth the statement sits at."""
    out, depth, cur, cur_at = [], 0, [], start
    for j in range(start, end + 1):
        c = code[j]
        if c in ';{}':
            text = ''.join(cur).strip()
            if text:
                out.append((code.count('\n', 0, cur_at) + 1, depth, ' '.join(text.split())))
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
            cur, cur_at = [], j + 1
        else:
            if not cur and c.isspace():
                cur_at = j + 1
                continue
            cur.append(c)
    return out


def declared_from_class(stmts, upto, name):
    """Is the nearest declaration of `name` above statement `upto` taken from char_slot / CharSlotOf?"""
    decl = re.compile(r'\b(?:int|int32_t|uint32_t|auto|bool)\s+' + re.escape(name) + r'\s*=(?!=)')
    for k in range(upto - 1, -1, -1):
        if decl.search(stmts[k][2]):
            return bool(re.search(r'\bchar_slot\b|\bCharSlotOf\s*\(', stmts[k][2]))
    return False


def through_class(stmts, k, text):
    if CLASS_DIRECT.search(text):
        return True
    for m in CLASS_NAMED.finditer(text):
        if 'char_slot' in m.group(1) or declared_from_class(stmts, k, m.group(1)):
            return True
    return False


def check(text):
    """-> (problems, uses): uses = [(line, kind, statement)]."""
    code = strip(text)
    span = record_body(code)
    if span is None:
        raise LookupError('FrontEnd::Record( not found in %s' % FILE)
    stmts = statements(code, *span)
    bad, uses = [], []
    for k, (line, depth, s) in enumerate(stmts):
        for call in CALL.finditer(s):
            readers = []   # (index, statement) of the order statements this call's result reaches
            if ORDER.search(s):
                readers.append((k, s))
            else:
                head = s[:call.start()]
                lm, mm = LOCAL.search(head), MEMBER.search(head)
                if lm:
                    word = re.compile(r'\b' + re.escape(lm.group(1)) + r'\b')
                    for q in range(k + 1, len(stmts)):
                        if stmts[q][1] < depth:
                            break
                        if word.search(stmts[q][2]) and ORDER.search(stmts[q][2]):
                            readers.append((q, stmts[q][2]))
                elif mm:
                    word = re.compile(r'\.' + re.escape(mm.group(1)) + r'\b')
                    for q in range(len(stmts)):
                        if q != k and word.search(stmts[q][2]) and ORDER.search(stmts[q][2]):
                            readers.append((q, stmts[q][2]))
            if not readers:
                uses.append((line, 'shader / variant', s))
                continue
            uses.append((line, 'order', s))
            if not PLANT.search(s[:call.start()]):
                bad.append('%s:%d: FrontEnd::Record: IsCharEffect( feeds an order key (%s) as the rule, not only as a test plant\'s input: '
                           'the character rank / turn is the object\'s class (piece_owner.h CharSlotOf, Item::char_slot; S111 SEECHAR)' % (FILE, line, readers[0][1][:90]))
                continue
            for q, rs in readers:
                if not through_class(stmts, q, rs):
                    bad.append('%s:%d: FrontEnd::Record: the order key that reads this IsCharEffect( (line %d: %s) is not handed through the class '
                               '(SeeCharacter(<char_slot>, ...) / BodyObject(<a name from char_slot>, ...); S111 SEECHAR)' % (FILE, line, stmts[q][0], rs[:90]))
    return bad, uses


def plant(text, lines):
    """The real file with `lines` inserted just inside Record's closing brace."""
    span = record_body(strip(text))
    return text[:span[1]] + '\n'.join(lines) + '\n' + text[span[1]:]


TWINS = [
    ('the pre-SEECHAR rank key (the effect is the rule)', True,
     ['  { SeeKey k; k.character = i.mesh->material >= 0 && IsCharEffect(i.model->materials[size_t(i.mesh->material)].shader); }']),
    ('the pre-FADECAW turn key (a local, no plant)', True,
     ['  { const int slot = i.char_slot;', '    const bool char_effect = i.mesh->material >= 0 && IsCharEffect(mt->shader);',
      '    const body_order::Turn turn = body_order::TurnOf(body_order::BodyObject(slot, char_effect), false); }']),
    ('a plant-guarded local read straight into the key (no class)', True,
     ['  { const bool ch = g_plant_see_effect.load() && IsCharEffect(mt->shader);', '    SeeKey k; k.character = ch; }']),
    ('a member carried to the key', True,
     ['  { it.char_fx = IsCharEffect(mt->shader); }', '  { SeeKey k; k.character = i.char_fx; }']),
    ('a class key whose slot is not CharSlotOf\'s', True,
     ['  { const int slot = obj.layer - 1;', '    SeeKey k; k.character = SeeCharacter(slot, g_plant_see_effect.load() && IsCharEffect(mt->shader)); }']),
    ('a shader / variant use (allowed)', False,
     ['  { const bool tp = mt && IsCharEffect(mt->shader) && family_cull::CharTwoPass(sorted, false, 0, true);', '    if (tp) ++n; }']),
]


def selftest(text):
    ok = True
    for name, must_fail, lines in TWINS:
        got, _ = check(plant(text, lines))
        if bool(got) != must_fail:
            print('selftest FAIL: twin "%s" %s' % (name, 'was not caught' if must_fail else 'was flagged: ' + '; '.join(got)[:200]))
            ok = False
    return ok


def main():
    root = pathlib.Path(sys.argv[sys.argv.index('--root') + 1]) if '--root' in sys.argv else pathlib.Path(__file__).resolve().parents[2]
    path = root / FILE
    if not path.is_file():
        print('char_class_key_lint: could not find %s' % path); return 2
    text = path.read_text(encoding='utf-8', errors='replace')
    try:
        bad, uses = check(text)
    except LookupError as e:
        print('char_class_key_lint: could not run: %s' % e); return 2
    for b in bad:
        print('FAIL ' + b)
    if '--verbose' in sys.argv:
        for line, kind, s in uses:
            print('  %s:%d %-16s %s' % (FILE, line, kind, s[:150]))
    ok = selftest(text) if not bad else True
    n_order = sum(1 for u in uses if u[1] == 'order')
    print('char_class_key_lint: %s (%d IsCharEffect( call(s) in FrontEnd::Record: %d order, %d shader / variant; %d problem(s))'
          % ('FAILED' if bad or not ok else 'ok', len(uses), n_order, len(uses) - n_order, len(bad) + (0 if ok else 1)))
    return 1 if bad or not ok else 0


if __name__ == '__main__':
    sys.exit(main())
