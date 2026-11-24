r"""S110 MEDIT: selftest of tools\edit\medit.py on fixtures built in temp folders, and of PLANTED TWINS of the tool (the pattern of
tools\seats\seats_selftest.py): each rule is first seen to FAIL against a copy of medit.py with one planted fault.

    py -I tools\edit\medit_selftest.py

Exit 0 = every check passes on the shipped tool and at least one check fails on every planted twin (a twin seen only through a crash of the
tool, a Traceback, is NOT caught unless its rule is "no crash"). The twins run in parallel child processes (`--suite PATH`). 0 game runs,
writes only under the temp folder.
"""
import concurrent.futures
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MEDIT = os.path.join(HERE, 'medit.py')

# (name, pattern in medit.py (\r?\n for a line break), replacement): one planted fault each
PLANTS = [
    ('many_matches_accepted', r"    if len\(starts\) > 1:\r?\n", "    if False:\n"),
    ('zero_matches_no_hint', r"out\.append\('    would match if leading", "('    would match if leading"),
    ('many_matches_no_lines', r"out\.append\('    line %d: ", "('    line %d: "),
    ('failure_still_writes', r"    if failures:\r?\n", "    if False:\n"),
    ('line_ending_matched_literally', r"NL = r'\\r\?\\n'", "NL = r'\\n'"),
    ('leading_blank_line_matches_at_cr_and_lf', r"return \('\(\?<!\\r\)' if find\.startswith\('\\n'\) else ''\) \+ ", "return "),
    ('crlf_not_kept_in_replacement', r"return text\[:s\] \+ _with_eols\(text, s, e, w, st\.eol\) \+ text\[e:\]", "return text[:s] + w + text[e:]"),
    ('bom_dropped', r"self\.bom, data = True, data\[3:\]", "self.bom, data = False, data[3:]"),
    ('final_newline_not_added_on_create', r"\(body \+ '\\n'\)\.replace", "(body).replace"),
    ('create_over_existing', r"        if st\.text is not None:\r?\n            raise Fail\('CREATE", "        if False:\n            raise Fail('CREATE"),
    ('non_utf8_replaced_silently', r"self\.text = data\.decode\('utf-8'\)", "self.text = data.decode('utf-8', 'replace')"),
    ('dotdot_allowed', r"if any\(part == '\.\.'", "if any(False"),
    ('outside_root_allowed', r"        if not inside:\r?\n", "        if False:\n"),
    ('symlink_not_resolved', r"real = os\.path\.realpath\(full\)", "real = full"),
    ('game_data_allowed', r"parts\[0\] in \('game_data', 'thirdparty'\)", "parts[0] in ('thirdparty',)"),
    ('generated_exception_lost', r"relnorm != 'generated\\\\rexglue\.cmake'", "True"),
    ('generated_not_refused', r"elif parts\[0\] == 'generated' and", "elif False and"),
    ('git_dir_allowed', r"if '\.git' in parts:", "if False:"),
    ('check_writes', r"    if check:\r?\n", "    if False:\n"),
    ('no_rollback', r"for st in reversed\(done\):", "for st in []:"),
    ('whole_line_delete_leaves_blank', r"    if line_start_at\(text, s\) and line_end_at\(text, e\):\r?\n", "    if False:\n"),
    ('insert_after_not_on_new_line', r"            elif line_end_at\(text, e\):\r?\n", "            elif False:\n"),
    ('replace_all_only_first', r"for s, e in reversed\(spans\):", "for s, e in reversed(spans[:1]):"),
    ('new_file_ignores_sibling_crlf', r"        e = majority_eol\(head\)\r?\n        if e:\r?\n", "        e = majority_eol(head)\n        if False:\n"),
    ('marker_in_text_swallowed', r"errors\.append\('editfile line %d: unexpected marker line", "('editfile line %d: unexpected marker line"),
    # S110 review (MEDIT): the rules the review added
    ('trailing_dot_or_space_allowed', r"if any\(x not in \('', '\.'\) and x\[-1\] in ' \.'", "if any(False"),
    ('colon_past_drive_allowed', r"if ':' in tail or \(drive and not os\.path\.isabs\(given\)\):", "if False:"),
    ('keyed_by_spelling_not_real_path', r"key = _nc\(real\)", "key = _nc(full)"),
    ('read_only_not_refused', r"if not os\.access\(self\.full, os\.W_OK\):", "if False:"),
    ('created_folders_left_behind', r"def _remove_dirs\(made\):\r?\n", "def _remove_dirs(made):\n    return\n"),
    ('userdata_allowed', r"parts\[2\]\.startswith\('userdata'\)", "False"),
    ('settings_json_allowed', r"parts\[1\]\.startswith\('settings'\) and parts\[1\]\.endswith\('\.json'\)", "False"),
    ('other_worktree_allowed', r"elif parts\[:2\] == \['\.claude', 'worktrees'\] and len\(parts\) > 2:", "elif False:"),
    ('rules_not_anchored_at_checkout_top', r"anchor_real = os\.path\.realpath\(git_top\(root\) or root\) if root_given else root_real", "anchor_real = root_real"),
    ('stdout_crashes_on_non_ascii', r"sys\.stdout\.reconfigure\(errors='backslashreplace'\)", "pass"),
    ('insert_after_break_anchor_glued', r"if text\[e - 1\] == '\\n' and \(e >= len\(text\) or text\[e\] not in '\\r\\n'\):", "if False:"),
    ('insert_before_break_anchor_glued', r"elif text\[s\] in '\\r\\n':", "elif False:"),
    ('context_line_ending_rewritten', r"out\.append\(me\[i\] if i is not None and i < len\(me\) else fb\)", "out.append(fb)"),
    ('control_char_in_with_written', r"    if bad:\r?\n", "    if False:\n"),
    ('no_already_applied_hint', r"        if m:\r?\n            lines\.append", "        if False:\n            lines.append"),
    ('no_edited_note', r"return ' \(in the text as edited", "return '' if 1 else ' (in the text as edited"),
    ('editfile_crlf_kept_in_bodies', r"lines = re\.split\(r'\\r\?\\n', text\)", "lines = text.split('\\n')"),
]

# a twin whose rule IS "no crash": caught by a Traceback in the output is the right catch
CRASH_IS_THE_RULE = {'stdout_crashes_on_non_ascii'}


class T:
    def __init__(self, medit):
        self.medit, self.res, self.tmp = medit, [], tempfile.mkdtemp(prefix='medit_t_')
        self.n = 0

    def root(self, files=None):
        self.n += 1
        r = os.path.join(self.tmp, 'r%d' % self.n)
        os.makedirs(r)
        for p, data in (files or {}).items():
            fp = os.path.join(r, p)
            os.makedirs(os.path.dirname(fp), exist_ok=True)
            with open(fp, 'wb') as f:
                f.write(data)
        return r

    def run(self, root, text, *extra, cwd=None, noroot=False):
        ef = os.path.join(self.tmp, 'edit%d.medit' % self.n)
        with open(ef, 'wb') as f:
            f.write(text if isinstance(text, bytes) else text.encode('utf-8'))
        cmd = [sys.executable, '-I', self.medit, ef] + ([] if noroot else ['--root', root]) + list(extra)
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120, cwd=cwd or root, encoding='utf-8', errors='replace')
        return r.returncode, r.stdout + r.stderr

    def read(self, root, p):
        with open(os.path.join(root, p), 'rb') as f:
            return f.read()

    def snap(self, root):
        out = {}
        for dp, dn, fn in os.walk(root):
            for f in fn:
                fp = os.path.join(dp, f)
                out[os.path.relpath(fp, root)] = open(fp, 'rb').read()
        return out

    def check(self, name, ok, detail=''):
        self.res.append((name, bool(ok), detail))


def B(op, path, find=None, with_=None, nowith=False):
    """one block of an editfile (built from parts so this file holds no marker line)"""
    P = '%%medit '
    s = [P + op + ' ' + path]
    if find is not None:
        s += [P + 'FIND', find]
    if op != 'DELETE' and find is None or find is not None:
        s += [P + 'WITH'] + ([] if nowith else [with_ if with_ is not None else ''])
    s.append(P + 'END')
    return '\n'.join(s) + '\n'


def load_module(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location('medit_under_test', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def suite(medit):
    t = T(medit)
    try:
        cases(t)
    finally:
        shutil.rmtree(t.tmp, ignore_errors=True)
    return t.res


def cases(t):
    # --- each op
    r = t.root({'a.txt': b'one\ntwo\nthree\n'})
    rc, out = t.run(r, B('REPLACE', 'a.txt', 'two', 'TWO\nTWO2'))
    t.check('replace once', rc == 0 and t.read(r, 'a.txt') == b'one\nTWO\nTWO2\nthree\n', out)
    t.check('output line: path, blocks, +A -R', 'a.txt: 1 block(s) applied, +2 -1 lines' in out and 'medit: 1 block(s) in 1 file(s) applied' in out, out)

    r = t.root({'a.txt': b'x\nfoo\ny\nfoo\nz foo\n'})
    rc, out = t.run(r, B('REPLACE-ALL', 'a.txt', 'foo', 'bar'))
    t.check('replace-all every occurrence + count', rc == 0 and t.read(r, 'a.txt') == b'x\nbar\ny\nbar\nz bar\n' and '3 occurrence' in out, out)
    rc, out = t.run(r, B('REPLACE-ALL', 'a.txt', 'nothere', 'q'))
    t.check('replace-all needs at least one', rc == 1 and 'NOTHING written' in out and '0 matches' in out, out)

    r = t.root({'a.txt': b'a\nb\nc\n'})
    rc, out = t.run(r, B('INSERT-AFTER', 'a.txt', 'b', 'b2') + B('INSERT-BEFORE', 'a.txt', 'c', 'c0'))
    t.check('insert-after / insert-before put WITH on new lines', rc == 0 and t.read(r, 'a.txt') == b'a\nb\nb2\nc0\nc\n', out + repr(t.read(r, 'a.txt')))
    r = t.root({'a.txt': b'int f(int a, int b);\n'})
    rc, out = t.run(r, B('INSERT-AFTER', 'a.txt', 'int a', ', int z'))
    t.check('insert-after an anchor inside a line is literal', rc == 0 and t.read(r, 'a.txt') == b'int f(int a, int z, int b);\n', repr(t.read(r, 'a.txt')))

    r = t.root({'dir/old.txt': b'keep\n'})
    rc, out = t.run(r, '%%medit CREATE dir/new.h\n%%medit WITH\nl1\nl2\n%%medit END\n')
    t.check('create: new file, content + final newline, LF without a CRLF sibling', rc == 0 and t.read(r, 'dir/new.h') == b'l1\nl2\n', out)
    rc, out = t.run(r, '%%medit CREATE dir/old.txt\n%%medit WITH\nx\n%%medit END\n')
    t.check('create over an existing file is refused, nothing written', rc == 1 and t.read(r, 'dir/old.txt') == b'keep\n' and 'already exists' in out, out)
    r = t.root({'d/s.h': b'a\r\nb\r\n'})
    rc, out = t.run(r, '%%medit CREATE d/n.h\n%%medit WITH\nq\nw\n%%medit END\n')
    t.check('create next to a CRLF sibling of the same extension is CRLF', rc == 0 and t.read(r, 'd/n.h') == b'q\r\nw\r\n', repr(t.read(r, 'd/n.h')) + out)

    r = t.root({'gone.txt': b'x\n'})
    rc, out = t.run(r, '%%medit DELETE gone.txt\n%%medit END\n')
    t.check('delete removes the file', rc == 0 and not os.path.exists(os.path.join(r, 'gone.txt')), out)
    rc, out = t.run(r, '%%medit DELETE gone.txt\n%%medit END\n')
    t.check('delete of a missing file is refused', rc == 1 and 'does not exist' in out, out)

    # --- all or none
    r = t.root({'a.txt': b'a1\na2\n', 'b.txt': b'b1\r\nb2\r\n', 'c.txt': b'c1\n'})
    before = t.snap(r)
    rc, out = t.run(r, B('REPLACE', 'a.txt', 'a1', 'A1') + B('REPLACE', 'b.txt', 'b2', 'B2') + B('CREATE', 'n.txt', None, 'x')
                    + B('REPLACE', 'c.txt', 'NOPE', 'x'))
    after = t.snap(r)
    t.check('all or none: a failing last block leaves every file byte-identical and no temp file', rc == 1 and before == after and 'NOTHING written: 1 block(s) failed' in out, out + str(sorted(after)))
    rc, out = t.run(r, B('REPLACE', 'a.txt', 'a1', 'A1') + B('REPLACE', 'b.txt', 'b2', 'B2'), '--check')
    t.check('--check writes nothing and reports', rc == 0 and t.snap(r) == before and 'would apply' in out, out)

    # --- line endings, BOM, final newline
    r = t.root({'w.h': b'int a;\r\nint b;\r\nint c;\r\n'})
    rc, out = t.run(r, B('REPLACE', 'w.h', 'int a;\nint b;', 'int A;\nint B;\nint B2;'))
    t.check('CRLF file, LF find: matches, new lines CRLF, other lines untouched', rc == 0 and t.read(r, 'w.h') == b'int A;\r\nint B;\r\nint B2;\r\nint c;\r\n', repr(t.read(r, 'w.h')) + out)
    r = t.root({'w2.h': b'x {\r\n\r\nnamespace q;\r\n'})
    rc, out = t.run(r, B('REPLACE', 'w2.h', '\nnamespace q;', '\nnamespace Q;'))
    t.check('CRLF: a FIND that starts with a blank line matches once (not at the CR and again at the LF)', rc == 0 and t.read(r, 'w2.h') == b'x {\r\n\r\nnamespace Q;\r\n', out + repr(t.read(r, 'w2.h')))
    r = t.root({'m.h': b'a\r\nb\r\nc\nd\r\n'})
    rc, out = t.run(r, B('REPLACE', 'm.h', 'b\nc\nd', 'b\nC\nd'))
    t.check('mixed endings: the match spans CRLF and LF; each line keeps its own ending, the changed line too', rc == 0 and t.read(r, 'm.h') == b'a\r\nb\r\nC\nd\r\n', repr(t.read(r, 'm.h')) + out)
    r = t.root({'m.h': b'a\r\nb\nc\r\nd\r\n'})
    rc, out = t.run(r, B('REPLACE', 'm.h', 'a\nb\nc', 'a\nb\nC'))
    t.check('mixed endings: a context line inside a multi-line FIND keeps its own ending (no rewrite of the matched span)', rc == 0 and t.read(r, 'm.h') == b'a\r\nb\nC\r\nd\r\n', repr(t.read(r, 'm.h')) + out)
    r = t.root({'m.h': b'a\r\nb\r\nc\r\nd\ne\n'})
    rc, out = t.run(r, B('REPLACE', 'm.h', 'c', 'C\nC2'))
    t.check('mixed endings: an edit leaves the other lines byte-identical (majority ending for the new line)', rc == 0 and t.read(r, 'm.h') == b'a\r\nb\r\nC\r\nC2\r\nd\ne\n', repr(t.read(r, 'm.h')))
    r = t.root({'u.txt': b'\xef\xbb\xbfhello\r\nworld'})
    rc, out = t.run(r, B('REPLACE', 'u.txt', 'hello', 'HELLO'))
    t.check('BOM kept, CRLF kept, no final newline kept', rc == 0 and t.read(r, 'u.txt') == b'\xef\xbb\xbfHELLO\r\nworld', repr(t.read(r, 'u.txt')))
    rc, out = t.run(r, B('REPLACE', 'u.txt', 'world', 'a\nb'))
    t.check('last line without a newline stays without one', rc == 0 and t.read(r, 'u.txt') == b'\xef\xbb\xbfHELLO\r\na\r\nb', repr(t.read(r, 'u.txt')))
    r = t.root({'t.py': b'\tif x:   \n\t\ty = 1\t\n'})
    rc, out = t.run(r, B('REPLACE', 't.py', '\t\ty = 1', '\t\ty = 2'))
    t.check('tabs and trailing spaces / tabs preserved', rc == 0 and t.read(r, 't.py') == b'\tif x:   \n\t\ty = 2\t\n', repr(t.read(r, 't.py')))
    r = t.root({'u8.txt': 'caf\u00e9 \u2192 x\n'.encode('utf-8')})
    rc, out = t.run(r, B('REPLACE', 'u8.txt', 'x', 'y'))
    t.check('UTF-8 text round-trips', rc == 0 and t.read(r, 'u8.txt') == 'caf\u00e9 \u2192 y\n'.encode('utf-8'), out)
    r = t.root({'bad.txt': b'abc \xff\xfe def\n'})
    rc, out = t.run(r, B('REPLACE', 'bad.txt', 'abc', 'x'))
    t.check('a non-UTF-8 file is refused and named', rc == 1 and 'not UTF-8' in out and 'bad.txt' in out and t.read(r, 'bad.txt') == b'abc \xff\xfe def\n', out)

    # --- same file, in order
    r = t.root({'o.txt': b'a\nb\n'})
    rc, out = t.run(r, B('REPLACE', 'o.txt', 'a', 'X') + B('REPLACE', 'o.txt', 'X\nb', 'Y\nb') + B('INSERT-AFTER', 'o.txt', 'Y', 'Z'))
    t.check('same-file blocks apply in order; each FIND sees the text as edited so far', rc == 0 and t.read(r, 'o.txt') == b'Y\nZ\nb\n' and 'o.txt: 3 block(s)' in out, out + repr(t.read(r, 'o.txt')))
    r = t.root({'o.txt': b'a\nb\n'})
    rc, out = t.run(r, B('REPLACE', 'o.txt', 'b', 'c') + B('REPLACE', 'o.txt', 'b', 'd'))
    t.check('a later FIND of text an earlier block removed fails', rc == 1 and '0 matches' in out and t.read(r, 'o.txt') == b'a\nb\n', out)

    # --- empty WITH deletes whole lines
    r = t.root({'d.txt': b'a\r\nb\r\nc\r\n'})
    rc, out = t.run(r, B('REPLACE', 'd.txt', 'b', nowith=True))
    t.check('an empty WITH deletes the whole line, line break included', rc == 0 and t.read(r, 'd.txt') == b'a\r\nc\r\n', repr(t.read(r, 'd.txt')) + out)
    r = t.root({'d.txt': b'a\nb\nc\n'})
    rc, out = t.run(r, B('REPLACE', 'd.txt', 'b', ''))
    t.check('a WITH of one blank line keeps the line break', rc == 0 and t.read(r, 'd.txt') == b'a\n\nc\n', repr(t.read(r, 'd.txt')))

    # --- failures are cheap to fix
    r = t.root({'h.cpp': b'void f() {\n    int total = a + b;\n    return total;\n}\n'})
    rc, out = t.run(r, B('REPLACE', 'h.cpp', '  int total = a + b;\n  return total;', 'x'))
    t.check('0 matches: the closest candidate, its line number, the whitespace verdict',
            rc == 1 and 'closest: line 2' in out and 'FIND: ' in out and 'FILE: ' in out and 'leading / trailing whitespace' in out and 'if leading / trailing whitespace were ignored: yes' in out, out)
    rc, out = t.run(r, B('REPLACE', 'h.cpp', '    int total = a + c;', 'x'))
    t.check('0 matches: a changed word is shown', rc == 1 and 'closest: line 2' in out and 'FIND "c" vs FILE "b"' in out and 'ignored: no' in out, out)
    r = t.root({'m.cpp': b'int a;\nfoo();\nint b;\nfoo();\nint c;\n'})
    rc, out = t.run(r, B('REPLACE', 'm.cpp', 'foo();', 'bar();'))
    t.check('2 matches: each match with its line number and the line before', rc == 1 and '2 matches' in out and 'line 2:' in out and 'line 4:' in out and 'line before: int b;' in out, out)
    t.check('2 matches: nothing written', t.read(r, 'm.cpp') == b'int a;\nfoo();\nint b;\nfoo();\nint c;\n')
    rc, out = t.run(r, '%%medit REPLACE m.cpp\n%%medit FIND\nfoo();\n%%medit FIND\nx\n%%medit WITH\ny\n%%medit END\n')
    t.check('a marker line where text was expected is a parse error, nothing written', rc == 1 and 'unexpected marker' in out and t.read(r, 'm.cpp').startswith(b'int a'), out)
    rc, out = t.run(r, '%%medit REPLACE nofile.cpp\n%%medit FIND\nx\n%%medit WITH\ny\n%%medit END\n')
    t.check('a missing file fails the block', rc == 1 and 'nofile.cpp' in out and 'NOTHING written' in out, out)

    # --- paths
    r = t.root({'sub/a.txt': b'x\n'})
    outside = os.path.join(t.tmp, 'outside%d' % t.n)
    os.makedirs(outside)
    with open(os.path.join(outside, 'o.txt'), 'wb') as f:
        f.write(b'secret\n')
    rc, out = t.run(r, B('REPLACE', 'sub/../a.txt', 'x', 'y'))
    t.check('a .. component is refused', rc == 1 and '".."' in out, out)
    rc, out = t.run(r, B('REPLACE', '../outside%d/o.txt' % t.n, 'secret', 'y'))
    t.check('a .. escape is refused, the outside file untouched', rc == 1 and open(os.path.join(outside, 'o.txt'), 'rb').read() == b'secret\n', out)
    rc, out = t.run(r, B('REPLACE', os.path.join(outside, 'o.txt'), 'secret', 'y'))
    t.check('an absolute path outside the root is refused', rc == 1 and 'outside the root' in out and open(os.path.join(outside, 'o.txt'), 'rb').read() == b'secret\n', out)
    rc, out = t.run(r, B('REPLACE', os.path.join(r, 'sub', 'a.txt'), 'x', 'y'))
    t.check('an absolute path inside the root is fine', rc == 0 and t.read(r, 'sub/a.txt') == b'y\n', out)
    # A junction / symlink escape: tools\lint\junction_lint.py forbids a tool creating one, so the check runs IN PROCESS on the tool's own
    # path resolver with os.path.realpath faked to follow root\junc to the outside folder (what the OS does for a real junction).
    mod = load_module(t.medit)
    jroot = os.path.realpath(r)
    real_realpath = os.path.realpath

    def fake_realpath(p, *a, **k):
        p = real_realpath(p, *a, **k)
        j = os.path.join(jroot, 'junc')
        return os.path.join(outside, os.path.relpath(p, j)) if os.path.normcase(p).startswith(os.path.normcase(j)) else p
    mod.os.path.realpath = fake_realpath
    try:
        try:
            mod.resolve_path(r, jroot, 'junc/o.txt')
            msg = None
        except mod.Fail as e:
            msg = str(e)
        except Exception as e:          # a twin that crashes here is a FAIL line, not a selftest crash
            msg = 'Traceback (in process): %r' % e
    finally:
        mod.os.path.realpath = real_realpath
    t.check('a junction / symlink escape (realpath leaves the root) is refused', msg is not None and 'junction' in msg, str(msg))
    r = t.root({'game_data/x.txt': b'q\n', 'generated/g.h': b'q\n', 'generated/rexglue.cmake': b'q\n', 'out/build/b.txt': b'q\n', 'out/other/o.txt': b'q\n',
                'thirdparty/t.h': b'q\n', '.git/config': b'q\n', 'src/ok.h': b'q\n', 'sub/.git/x': b'q\n'})
    for p, refused in (('game_data/x.txt', True), ('generated/g.h', True), ('generated/rexglue.cmake', False), ('out/build/b.txt', True), ('out/other/o.txt', False),
                       ('thirdparty/t.h', True), ('.git/config', True), ('src/ok.h', False), ('sub/.git/x', True)):
        rc, out = t.run(r, B('REPLACE', p, 'q', 'z'))
        got = t.read(r, p)
        t.check('folder rule: %s %s' % (p, 'refused' if refused else 'allowed'), (rc == 1 and got == b'q\n' and 'never edited' in out) if refused else (rc == 0 and got == b'z\n'), out)

    # --- the root is the git top level of the cwd
    r = t.root({'g/a.txt': b'x\n'})
    if subprocess.run(['git', 'init', '-q', r], capture_output=True).returncode == 0:
        rc, out = t.run(r, B('REPLACE', 'g/a.txt', 'x', 'y'), cwd=os.path.join(r, 'g'), noroot=True)
        t.check('no --root: the git top level of the current folder (run from a subfolder)', rc == 0 and t.read(r, 'g/a.txt') == b'y\n', out)

    # --- a rename that fails part-way restores the files already renamed (Windows: an open handle blocks os.replace)
    if os.name == 'nt':
        r = t.root({'a.txt': b'a\n', 'b.txt': b'b\n'})
        hold = open(os.path.join(r, 'b.txt'), 'rb')
        try:
            rc, out = t.run(r, B('REPLACE', 'a.txt', 'a', 'A') + B('REPLACE', 'b.txt', 'b', 'B'))
        finally:
            hold.close()
        names = sorted(os.listdir(r))
        t.check('a rename failing part-way restores the renamed files', rc == 1 and t.read(r, 'a.txt') == b'a\n' and t.read(r, 'b.txt') == b'b\n' and names == ['a.txt', 'b.txt'], out + str(names))
    else:
        print('  (rename failure: needs a locked file, Windows only: SKIPPED)')

    # --- S110 review: one file under two spellings is ONE file; names Windows rewrites are refused
    r = t.root({'a.txt': b'one\ntwo\n'})
    rc, out = t.run(r, B('REPLACE', 'a.txt', 'one', 'ONE') + B('REPLACE', 'a.txt.', 'two', 'TWO'))
    t.check('a trailing dot (Windows drops it: a second name for a.txt) is refused, nothing written', rc == 1 and 'dot or a space' in out and t.read(r, 'a.txt') == b'one\ntwo\n', out)
    r = t.root({'longfoldername/a.txt': b'one\ntwo\n'})
    if os.path.isdir(os.path.join(r, 'LONGFO~1')):
        rc, out = t.run(r, B('REPLACE', 'longfoldername/a.txt', 'one', 'ONE') + B('REPLACE', 'LONGFO~1/a.txt', 'two', 'TWO'))
        t.check('an 8.3 short name and the long name are ONE file: both blocks land', rc == 0 and t.read(r, 'longfoldername/a.txt') == b'ONE\nTWO\n' and '2 block(s) in 1 file(s)' in out, out)
    else:
        print('  (8.3 short names are off on this volume: SKIPPED)')
    r = t.root({'a.txt': b'x\n'})
    for p in ('a.txt:hidden', 'C:a.txt'):
        rc, out = t.run(r, B('CREATE', p, None, 'q'))
        t.check('a ":" past the drive (a data stream, a drive-relative path) is refused: %s' % p, rc == 1 and '":"' in out and t.snap(r) == {'a.txt': b'x\n'}, out)

    # --- a read-only file is refused before anything is written (it used to fail at the rename and leave its temp file behind)
    r = t.root({'a.txt': b'a\n', 'ro.txt': b'r\n'})
    os.chmod(os.path.join(r, 'ro.txt'), stat.S_IREAD)
    before = t.snap(r)
    try:
        rc, out = t.run(r, B('REPLACE', 'a.txt', 'a', 'A') + B('REPLACE', 'ro.txt', 'r', 'R'))
        after = t.snap(r)
    finally:
        for f in os.listdir(r):
            os.chmod(os.path.join(r, f), stat.S_IREAD | stat.S_IWRITE)
    t.check('a read-only file is refused in the resolve phase; every file byte-identical, no temp file', rc == 1 and 'read-only' in out and before == after, out + str(sorted(after)))

    # --- a failed write removes the folders a CREATE made
    if os.name == 'nt':
        r = t.root({'b.txt': b'b\n'})
        hold = open(os.path.join(r, 'b.txt'), 'rb')
        try:
            rc, out = t.run(r, B('CREATE', 'new/deep/n.txt', None, 'q') + B('REPLACE', 'b.txt', 'b', 'B'))
        finally:
            hold.close()
        t.check('a rename failing part-way also removes the folders a CREATE made', rc == 1 and sorted(os.listdir(r)) == ['b.txt'] and t.read(r, 'b.txt') == b'b\n', out + str(os.listdir(r)))

    # --- the refused list: the frozen user data, the owner's permission rules, another checkout's tree
    r = t.root({'out/bench/userdata_frozen/s.txt': b'q\n', 'out/bench/userdata_wm30open_s101/s.txt': b'q\n', 'out/bench/reference/r.csv': b'q\n',
                '.claude/settings.json': b'q\n', '.claude/settings.local.json': b'q\n', '.claude/worktrees/agent-x/a.txt': b'q\n', '.claude/agents/a.md': b'q\n'})
    for p, refused in (('out/bench/userdata_frozen/s.txt', True), ('out/bench/userdata_wm30open_s101/s.txt', True), ('out/bench/reference/r.csv', False),
                       ('.claude/settings.json', True), ('.claude/settings.local.json', True), ('.claude/worktrees/agent-x/a.txt', True), ('.claude/agents/a.md', False)):
        rc, out = t.run(r, B('REPLACE', p, 'q', 'z'))
        got = t.read(r, p)
        t.check('folder rule: %s %s' % (p, 'refused' if refused else 'allowed'), (rc == 1 and got == b'q\n' and 'never edited' in out) if refused else (rc == 0 and got == b'z\n'), out)
    r = t.root({'game_data/g.txt': b'q\n'})
    if subprocess.run(['git', 'init', '-q', r], capture_output=True).returncode == 0:
        rc, out = t.run(os.path.join(r, 'game_data'), B('REPLACE', 'g.txt', 'q', 'z'))
        t.check('--root inside a refused folder of a checkout: still refused (the rules are anchored at the checkout top)', rc == 1 and 'never edited' in out and t.read(r, 'game_data/g.txt') == b'q\n', out)

    # --- output: a non-ASCII hint on a cp1252 pipe does not crash the tool
    r = t.root({'u.txt': 'café → x\n'.encode('utf-8')})
    rc, out = t.run(r, B('REPLACE', 'u.txt', 'café → y', 'z'))
    t.check('a non-ASCII 0-match hint prints on a cp1252 pipe (no crash)', rc == 1 and '0 matches' in out and 'closest' in out and 'Traceback' not in out, out)

    # --- INSERT anchors that hold a line break at their edge put WITH on its own line
    r = t.root({'a.txt': b'a\nb\nc\n'})
    rc, out = t.run(r, B('INSERT-AFTER', 'a.txt', 'b\n', 'X'))
    t.check('insert-after an anchor that ends with its line break: WITH on its own line (not glued to the next line)', rc == 0 and t.read(r, 'a.txt') == b'a\nb\nX\nc\n', repr(t.read(r, 'a.txt')) + out)
    r = t.root({'a.txt': b'a\nb\n'})
    rc, out = t.run(r, B('INSERT-AFTER', 'a.txt', 'b\n', 'X'))
    t.check('insert-after an anchor that ends with the last line break: WITH is the new last line', rc == 0 and t.read(r, 'a.txt') == b'a\nb\nX\n', repr(t.read(r, 'a.txt')) + out)
    r = t.root({'a.txt': b'a\r\nb\r\nc\r\n'})
    rc, out = t.run(r, B('INSERT-BEFORE', 'a.txt', '\nb', 'X'))
    t.check('insert-before an anchor that starts with a line break: WITH on its own line (not glued to the line before)', rc == 0 and t.read(r, 'a.txt') == b'a\r\nX\r\nb\r\nc\r\n', repr(t.read(r, 'a.txt')) + out)

    # --- a control character in a WITH is refused (tools\lint\no_control_chars.py's rule; a \b through a string, a lone CR)
    for bad in ('a\x00b', 'a\rb', 'a\x08b'):
        r = t.root({'a.txt': b'x\n'})
        rc, out = t.run(r, B('REPLACE', 'a.txt', 'x', bad))
        t.check('a control character in a WITH (%r) is refused, nothing written' % bad, rc == 1 and 'control character' in out and t.read(r, 'a.txt') == b'x\n', out)

    # --- the 0-match hint: already applied; checked against the text as edited by an earlier block
    r = t.root({'h.cpp': b'void f();\n    int a = 1;\n'})
    rc, out = t.run(r, B('REPLACE', 'h.cpp', '    int a = 0;', '    int a = 1;'))
    t.check('0 matches while the WITH is in the file: "already applied?" with its line', rc == 1 and 'already applied?' in out and 'at line 2' in out, out)
    r = t.root({'h.cpp': b'a\nb\n'})
    rc, out = t.run(r, B('REPLACE', 'h.cpp', 'a', 'A') + B('REPLACE', 'h.cpp', 'a\nb', 'x'))
    t.check('0 matches after an earlier block of the same file names that block', rc == 1 and 'as edited by block(s) 1' in out, out)

    # --- the editfile itself: CRLF bodies against an LF file, regex-like and backslash text taken literally, REPLACE-ALL never re-matches its output
    r = t.root({'a.txt': b'a\nb\nc\n'})
    rc, out = t.run(r, ('%%medit REPLACE a.txt\r\n%%medit FIND\r\na\r\nb\r\n%%medit WITH\r\nA\r\nB\r\nB2\r\n%%medit END\r\n').encode('utf-8'))
    t.check('a CRLF editfile against an LF file writes LF only', rc == 0 and t.read(r, 'a.txt') == b'A\nB\nB2\nc\n', repr(t.read(r, 'a.txt')) + out)
    r = t.root({'a.txt': b'x x\n'})
    rc, out = t.run(r, B('REPLACE-ALL', 'a.txt', 'x', r'x\1\g<0>$1 src\new'))
    t.check('REPLACE-ALL: a WITH holding its FIND, regex groups and backslashes is inserted literally, once per match', rc == 0 and t.read(r, 'a.txt') == b'x\\1\\g<0>$1 src\\new x\\1\\g<0>$1 src\\new\n', repr(t.read(r, 'a.txt')) + out)
    r = t.root({'a.txt': b'old\n'})
    rc, out = t.run(r, B('DELETE', 'a.txt') + B('CREATE', 'a.txt', None, 'new') + B('CREATE', 'b.txt', None, 'b'))
    t.check('DELETE then CREATE of one path in one editfile writes the new content', rc == 0 and t.read(r, 'a.txt') == b'new\n' and t.read(r, 'b.txt') == b'b\n', out)
    before = t.snap(r)
    rc, out = t.run(r, B('CREATE', 'c.txt', None, 'c') + B('CREATE', 'c.txt', None, 'c2'))
    t.check('two CREATEs of one path: the second fails, nothing written', rc == 1 and 'already exists' in out and t.snap(r) == before, out)
    rc, out = t.run(r, B('DELETE', 'a.txt') + B('REPLACE', 'a.txt', 'new', 'x'))
    t.check('an edit of a file an earlier block DELETEd fails, nothing written', rc == 1 and 'does not exist' in out and t.snap(r) == before, out)


def twin(src_text, name, pat, rep):
    new, n = re.subn(pat, lambda m: rep, src_text, count=1)
    return new if n == 1 else None


def suite_in_child(path):
    """the suite on one twin in its own process: -> [(name, ok, detail)], or None when the child crashed"""
    r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--suite', path], capture_output=True, text=True, timeout=900,
                       encoding='utf-8', errors='replace')
    res = []
    for x in r.stdout.splitlines():
        try:
            res.append(tuple(json.loads(x)))
        except ValueError:
            pass
    return res if r.returncode == 0 and res else None


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--suite':
        for row in suite(sys.argv[2]):
            print(json.dumps(list(row)))
        return 0
    src = open(MEDIT, encoding='utf-8', newline='').read()
    problems = []
    res = suite(MEDIT)
    for name, ok, detail in res:
        print('%s  %s' % ('PASS' if ok else 'FAIL', name))
        if not ok:
            print('      ' + detail.strip().replace('\n', '\n      ')[:600])
            problems.append('shipped tool fails: ' + name)
    tmp = tempfile.mkdtemp(prefix='medit_twin_')
    caught = 0
    try:
        jobs = []
        for name, pat, rep in PLANTS:
            new = twin(src, name, pat, rep)
            if new is None:
                problems.append('plant %s: the pattern did not match exactly once (medit.py changed under it)' % name)
                continue
            p = os.path.join(tmp, name + '.py')
            with open(p, 'w', encoding='utf-8', newline='') as f:
                f.write(new)
            jobs.append((name, p))
        with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 2)) as ex:
            results = list(ex.map(lambda j: suite_in_child(j[1]), jobs))
        for (name, p), res in zip(jobs, results):
            if res is None:
                problems.append('plant %s: the selftest child crashed' % name)
                print('twin %-36s CHILD CRASHED' % name)
                continue
            failed = [(n, d) for n, ok, d in res if not ok]
            crash_only = bool(failed) and all('Traceback' in d for _, d in failed) and name not in CRASH_IS_THE_RULE
            if failed and not crash_only:
                caught += 1
            elif crash_only:
                problems.append('plant %s: seen only through a crash of the tool (Traceback), no check of its rule FAILs' % name)
            else:
                problems.append('plant %s: NOT CAUGHT (every check still passes)' % name)
            print('twin %-36s %s%s' % (name, ('caught by %d check(s)%s' % (len(failed), ' ONLY BY A CRASH' if crash_only else '')) if failed else 'NOT CAUGHT',
                                         ': ' + failed[0][0] if failed else ''))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    for p in problems:
        print('PROBLEM: ' + p)
    print('medit_selftest: %s (%d checks, %d/%d twins caught)' % ('OK' if not problems else '%d problem(s)' % len(problems), len(res), caught, len(PLANTS)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
