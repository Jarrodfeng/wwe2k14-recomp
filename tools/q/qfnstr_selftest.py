"""Selftest for qfnstr (S110 lane FNSTR). 0 game runs. Synthetic part always; the real part SKIPs without an index.
Each rule has a case that must NOT name. Exit 1 = a check failed."""
import os
import sqlite3
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qfnstr  # noqa: E402

fails = []


def check(what, got, want):
    ok = got == want
    print('%s %s: %r%s' % ('ok  ' if ok else 'FAIL', what, got, '' if ok else ' (want %r)' % (want,)))
    if not ok:
        fails.append(what)


def world(strings, refs, nofunc=()):
    """strings {addr: text}, refs [(func, string addr)] -> an in-memory index with the three tables qfnstr reads"""
    c = sqlite3.connect(':memory:')
    c.execute('create table strings(addr integer primary key, enc text, text text)')
    c.execute('create table strrefs(func integer, site integer, addr integer, how text, op text)')
    c.execute('create table funcs(addr integer, has_body integer)')
    c.executemany('insert into strings values (?,?,?)', [(a, 'ascii', t) for a, t in strings.items()])
    c.executemany('insert into strrefs values (?,?,?,?,?)', [(f, 0, a, 'direct', 'x') for f, a in refs])
    c.executemany('insert into funcs values (?,?)', [(f, 0 if f in nofunc else 1) for f in set(f for f, _ in refs) | set(nofunc)])
    return c


def got(strings, refs, **kw):
    return {(f, n, s) for f, n, s, a in qfnstr.rows(world(strings, refs, **kw))}


def main():
    one = 'void __cdecl CStage::TextureSetup(class CData *,TEX_GROUPLIST *)'
    # rule one_ref: one function -> a name; the name is the qualified name only
    check('one_ref names', got({1: one}, [(0x100, 1)]), {(0x100, 'CStage::TextureSetup', 'fn_string')})
    check('three-part name, return type with ::', got({1: 'enum A::R __thiscall NS::C::Go(int)'}, [(0x100, 1)]),
          {(0x100, 'NS::C::Go', 'fn_string')})
    check('bare Class::Method is the whole string', got({1: 'CFoo::Bar'}, [(0x100, 1)]), {(0x100, 'CFoo::Bar', 'fn_string')})
    check('no parameter list is allowed', got({1: 'void __cdecl CFoo::Bar'}, [(0x100, 1)]), {(0x100, 'CFoo::Bar', 'fn_string')})
    # must NOT name: not a signature
    check('plain text with :: inside is not a signature', got({1: 'see CFoo::Bar for details'}, [(0x100, 1)]), set())
    check('no qualifier: a free function is not named', got({1: 'void __cdecl Free(int)'}, [(0x100, 1)]), set())
    check('whole rule: a template or operator tail is dropped', got({1: 'void __cdecl CFoo<int>::Bar(int)', 2: 'bool __cdecl CFoo::operator==(int)'},
                                                                     [(0x100, 1), (0x101, 2)]), set())
    check('a clipped name before the parameters is dropped', got({1: 'void __cdecl CFoo::Ba~'}, [(0x100, 1)]), set())
    # rule shared: 2..4 functions -> candidates, never a name
    sh = got({1: one}, [(0x100, 1), (0x101, 1)])
    check('shared: both candidates', sh, {(0x100, 'CStage::TextureSetup', 'fn_string_shared'), (0x101, 'CStage::TextureSetup', 'fn_string_shared')})
    check('shared: 4 refs still candidates, none a name', {s for _, _, s in got({1: one}, [(0x100 + i, 1) for i in range(4)])}, {'fn_string_shared'})
    # rule hub: 5 or more -> nothing stored
    check('hub: 5 refs store nothing', got({1: one}, [(0x100 + i, 1) for i in range(5)]), set())
    # rule multi: two different qualified names in one function -> candidates, none a name
    two = {1: one, 2: 'void __cdecl CStage::LightSetup(int)'}
    check('multi: both strings candidates', got(two, [(0x100, 1), (0x100, 2)]),
          {(0x100, 'CStage::TextureSetup', 'fn_string_shared'), (0x100, 'CStage::LightSetup', 'fn_string_shared')})
    check('multi: no fn_string row for that function', {s for _, _, s in got(two, [(0x100, 1), (0x100, 2)])}, {'fn_string_shared'})
    check('same name twice is not multi', got({1: one, 2: 'void __cdecl CStage::TextureSetup(int)'}, [(0x100, 1), (0x100, 2)]),
          {(0x100, 'CStage::TextureSetup', 'fn_string')})
    check('multi does not touch the other functions', got({1: one, 2: 'void __cdecl CStage::LightSetup(int)'}, [(0x100, 1), (0x100, 2), (0x200, 2)]),
          {(0x100, 'CStage::TextureSetup', 'fn_string_shared'), (0x100, 'CStage::LightSetup', 'fn_string_shared'),
           (0x200, 'CStage::LightSetup', 'fn_string_shared')})
    # a reference from an address that is not a function body is not counted
    check('a ref outside a function body counts nothing', got({1: one}, [(0x100, 1)], nofunc=(0x100,)), set())
    st = qfnstr.analyze(world(two, [(0x100, 1), (0x100, 2), (0x300, 1)]))[1]
    check('stats: strings, multi', (st['signature_strings'], st['multi_funcs']), (2, 1))

    import qnames
    if not (os.path.exists(qnames.INDEX_DB) and os.path.exists(qnames.IMAGE)):
        print('SKIP real part: no index')
    else:
        # the five names PILOT1.md cites for stage_setup; the first through the real q name
        out = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'q.py'), 'name', '0x824E7AE8'], capture_output=True, text=True).stdout
        check('q name 0x824E7AE8 is CStage::TextureSetup, fn_string', 'CStage::TextureSetup' in out and 'fn_string ' in out and 'candidate (shared' not in out, True)
        rs = {f: (n, s) for f, n, s, a in qfnstr.rows(qnames.ro(qnames.INDEX_DB))}
        for f, w in ((0x824ECAD8, 'CStage::EtcObjSetup'), (0x824E8E38, 'CStage::LightSetupPlatform'),
                     (0x824E9050, 'CStage::LightSetup_AreaLight'), (0x824EADA8, 'CStage::CameraCollisionSetup')):
            n = rs.get(f, ('', ''))[0]
            check('sub_%08X resolves near %s' % (f, w), n.startswith('CStage::') and n[:20] == w[:20], True)
    print('FAILED: %s' % fails if fails else 'all passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
