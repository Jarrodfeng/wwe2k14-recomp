r"""Function names the game carries inside its own signature strings (S110 lane FNSTR). 0 game runs.

    py tools/q/qfnstr.py --stats     the counts of every rule
    py tools/q/qfnstr.py --list      the names (one_ref) and the candidates

The game keeps compiler signature strings for asserts and logs: `void __cdecl CStage::TextureSetup(class CData *,...)`.
A function that is the only reference of such a string very likely IS that function. Rules (each is one function below):
  one_ref      the signature string has exactly 1 referencing function            -> source fn_string (a name)
  shared       2..4 referencing functions: a helper inlined into callers, or an assert of overloads
               -> every one gets the name as a CANDIDATE only, source fn_string_shared
  hub          5 or more referencing functions: a log helper's format             -> not a name, counted, not stored
  multi        a function that references two or more different qualified names (an inlined callee's assert sits in the
               caller) -> all of them candidates, none its name
  whole        a string is read only if the qualified name is followed by `(`, by the end, or by nothing but spaces: a
               clipped name, or an operator / template we do not parse, is dropped (the index does not clip strings
               below 691 characters, but the check does not rely on that)
Output rows: (func, name, source, string_addr). The name is the qualified name only (`CStage::TextureSetup`).
Soundness is the bar: when a rule is in doubt the function gets a candidate, not a name.
"""
import collections
import re
import sys

CONV = r'(?:__cdecl|__thiscall|__stdcall|__fastcall|__clrcall|__vectorcall)'
IDENT = r'[A-Za-z_~][A-Za-z0-9_]*'
QUAL = r'%s(?:::%s)+' % (IDENT, IDENT)                 # A::B, A::B::C
SIG = re.compile(r'^(?:.*?[\s*&])?%s\s+(?P<q>%s)(?P<tail>\s*\(.*|\s*)$' % (CONV, QUAL), re.S)
BARE = re.compile(r'^(?P<q>%s)$' % QUAL)

ONE, SHARED = 'fn_string', 'fn_string_shared'
MAX_SHARED = 4


def parse(text):
    """-> (qualified name, kind) for a signature string, else None. kind 'sig' (convention word) or 'bare' (whole string)."""
    m = BARE.match(text)
    if m:
        return m.group('q'), 'bare'
    m = SIG.match(text)
    if m and (m.group('tail').lstrip().startswith('(') or not m.group('tail').strip()):
        return m.group('q'), 'sig'
    return None


def signature_strings(con):
    """{string addr: (qualified name, kind)} for every ascii string in the index that parses; plus a count of
    convention-word strings that did not parse (dropped by the `whole` rule)."""
    out, dropped = {}, 0
    for addr, text in con.execute("select addr,text from strings where enc='ascii'"):
        p = parse(text)
        if p:
            out[addr] = p
        elif re.search(CONV + r'\s+\S*::', text):
            dropped += 1
    return out, dropped


def refs_by_string(con, sigs):
    """{string addr: set of referencing functions with a body} for the signature strings only."""
    bodies = set(r[0] for r in con.execute('select addr from funcs where has_body=1'))
    refs = collections.defaultdict(set)
    for f, a in con.execute('select func,addr from strrefs'):
        if a in sigs and f in bodies:
            refs[a].add(f)
    return refs


def classify(sigs, refs):
    """the rules in order. -> (rows, stats)"""
    st = collections.Counter()
    st['signature_strings'] = len(sigs)
    st['sig_with_convention'] = sum(1 for q, k in sigs.values() if k == 'sig')
    st['sig_bare'] = sum(1 for q, k in sigs.values() if k == 'bare')
    st['strings_unreferenced'] = sum(1 for a in sigs if not refs.get(a))
    usable = {}                                                      # string -> funcs, for strings that may name or suggest
    for a in sorted(sigs):
        fs = refs.get(a, ())
        if not fs:
            continue
        if len(fs) > MAX_SHARED:
            st['rejected_hub_strings'] += 1                          # rule hub
            continue
        usable[a] = sorted(fs)
    names_of = collections.defaultdict(set)                          # func -> qualified names it references
    for a, fs in usable.items():
        for f in fs:
            names_of[f].add(sigs[a][0])
    rows = []
    multi = set(f for f, ns in names_of.items() if len(ns) >= 2)     # rule multi
    st['multi_funcs'] = len(multi)
    for a, fs in usable.items():
        q = sigs[a][0]
        if len(fs) == 1:
            f = fs[0]
            if f in multi:
                rows.append((f, q, SHARED, a))
                st['rejected_multi_one_ref'] += 1
            else:
                rows.append((f, q, ONE, a))
                st['named_one_ref'] += 1
        else:
            for f in fs:
                rows.append((f, q, SHARED, a))
                st['candidate_shared'] += 1
    st['candidate_rows'] = sum(1 for r in rows if r[2] == SHARED)
    st['candidate_funcs'] = len(set(r[0] for r in rows if r[2] == SHARED))
    st['named_funcs'] = len(set(r[0] for r in rows if r[2] == ONE))
    st['named_bare'] = sum(1 for r in rows if r[2] == ONE and sigs[r[3]][1] == 'bare')
    # one function, one name: two one_ref strings of the same function with the same qualified name collapse
    st['same_name_to_two_funcs'] = sum(1 for q, c in collections.Counter(r[1] for r in rows if r[2] == ONE).items() if c > 1)
    return sorted(set(rows)), st


def analyze(con):
    sigs, dropped = signature_strings(con)
    rows, st = classify(sigs, refs_by_string(con, sigs))
    st['rejected_not_whole'] = dropped
    return rows, st


def rows(index_con):
    """[(func, name, source, string_addr)] for qnames.build."""
    return analyze(index_con)[0]


def main(argv):
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import qnames
    con = qnames.ro(qnames.INDEX_DB)
    rs, st = analyze(con)
    if '--list' in argv:
        for f, n, s, a in rs:
            print('sub_%08X  %-50s %-17s str 0x%08X' % (f, n, 'name' if s == ONE else 'candidate', a))
    if '--stats' in argv or '--list' not in argv:
        print('signature strings found: %d (%d with a convention word, %d bare Class::Method); %d never referenced'
              % (st['signature_strings'], st['sig_with_convention'], st['sig_bare'], st['strings_unreferenced']))
        print('functions named (one_ref): %d (from %d strings, %d of them bare)' % (st['named_funcs'], st['named_one_ref'], st['named_bare']))
        print('candidates (fn_string_shared): %d functions, %d rows' % (st['candidate_funcs'], st['candidate_rows']))
        print('rejected: hub strings (5+ refs) %d | functions with 2+ different names %d (their one_ref rows %d demoted to candidates)'
              ' | not whole / unparsed %d' % (st['rejected_hub_strings'], st['multi_funcs'], st['rejected_multi_one_ref'], st['rejected_not_whole']))
        print('one qualified name on more than one named function (overloads): %d' % st['same_name_to_two_funcs'])
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
