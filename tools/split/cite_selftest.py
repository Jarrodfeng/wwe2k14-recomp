"""cite_selftest: the planted cases of cite_by_name (called by `cite_by_name.py --selftest`).  Every rule has a planted twin that must
change the result when the rule is reverted."""
import tempfile
from pathlib import Path

import cite_by_name as C

BIG = ("int a1;\nint a2;\n#include \"parts/p1.inc\"  // split: p1\nint z1;\nint z2;\n"
       "#include \"parts/p2.inc\"  // split: p2\nint end;\nint PadFn() {\n" + "  int x;\n" * 99 + "}\n")
SMALL = ("int Plain() {\n  return 0;\n}\n\nint Counter() {\n  log(\"census total\");\n  return g_zeta_count;\n}\n\nint Other() {\n  return 5;\n}\n"
         "struct Cfg {\n  int kOneOpt = 1;\n  int kTwoOpt = 2;\n};\n"          # kTwoOpt: line 15
         "static const int kTableOne[] = {\n  11,\n  kRowWord,\n  22,\n};\n")   # kRowWord: line 19
# expanded big.cpp: 1 a1, 2 a2, 3 AlphaFn{, 4 return g_alpha_val, 5 }, 6 z1, 7 z2, 8 BetaFn{, 9 ret, 10 }, 11 end, 12 PadFn{ ...
DOC = ("named `BetaFn` at big.cpp:4\n"                                   # name: line drifted
       "agree `AlphaFn` (big.cpp:4)\n"                                    # name beside, line agrees
       "drifted `BetaFn` returns two (big.cpp:3)\n"                       # name before, not beside: the name is in the sentence, never repeated
       "word: sets `g_alpha_val` (big.cpp:4)\n"                           # no function name: enclosing AlphaFn holds g_alpha_val
       "no word: sets `g_other_thing` (big.cpp:4)\n"                      # enclosing holds none of the words: the file only (the PARENT: the stale line's part may be wrong too)
       "log \"census total\" is printed (small.cpp:6)\n"                  # quoted log text found inside Counter
       "d3_binder.h:2 is another file, binder.h:2 is ours, `BinderFn`\n"  # left boundary; the label BinderFn is in the sentence
       "sizes `PadFn` big.cpp 100 -> 7, big.cpp 100 lines\n"              # a size is no line
       "tail `BetaFn` keeps it (big.cpp:9 / 500)\n"                       # a tail item past the end: the parent file
       "a member: the row sets `kTwoOpt` (small.cpp:15)\n"                # inside a type: the word on the line beats the type
       "the row sets `kRowWord` (small.cpp:19)\n"                         # no function: the top-level table
       "prose with no names at all (big.cpp:200)\n"                       # past the end: the file
       "`BetaFn` here (big.cpp:9), `:12` there\n"                         # a bare :N follow-on
       "prose (`big.cpp:200` and `:300`) both\n"                          # a dropped follow-on takes its joiner with it
       "prose big.cpp:200 first, :300 second\n"                           # a dropped follow-on leaves one space, not two
       "prose (big.cpp:200) and the hold (`:300`) too\n"                  # a dropped follow-on leaves no empty parentheses
       "shader sets `out_colour` (s.frag:3)\n"                            # a shader's main is no name, by either path
       "sdk: src/graphics/gfx.cpp:5 is the SDK's\n"                       # an SDK path is not ours and not gone
       "gone: src/x/gone.h:5 is no file\n")                               # a ref to a file that no longer exists: flagged, left
WANT = ("named `BetaFn` at parts/p2.inc\n"
        "agree `AlphaFn` (parts/p1.inc)\n"
        "drifted `BetaFn` returns two (parts/p2.inc)\n"
        "word: sets `g_alpha_val` (parts/p1.inc (AlphaFn))\n"
        "no word: sets `g_other_thing` (big.cpp)\n"
        "log \"census total\" is printed (small.cpp (Counter))\n"
        "d3_binder.h:2 is another file, binder.h is ours, `BinderFn`\n"
        "sizes `PadFn` big.cpp 100 -> 7, big.cpp 100 lines\n"
        "tail `BetaFn` keeps it (parts/p2.inc / big.cpp)\n"
        "a member: the row sets `kTwoOpt` (small.cpp)\n"
        "the row sets `kRowWord` (small.cpp (kTableOne))\n"
        "prose with no names at all (big.cpp)\n"
        "`BetaFn` here (parts/p2.inc), `big.cpp` there\n"
        "prose (`big.cpp`) both\n"
        "prose big.cpp first, second\n"
        "prose (big.cpp) and the hold too\n"
        "shader sets `out_colour` (s.frag)\n"
        "sdk: src/graphics/gfx.cpp:5 is the SDK's\n"
        "gone: src/x/gone.h:5 is no file\n")
CODE = ("int v = 1;  // see big.cpp:4 (BetaFn) for it\n"
        "const char* s = \"`AlphaFn` at big.cpp:4 stays in a string\";\n"
        "/* block: `AlphaFn` at big.cpp:4 */\n")
WANT_CODE = ("int v = 1;  // see parts/p2.inc (BetaFn) for it\n"
             "const char* s = \"`AlphaFn` at big.cpp:4 stays in a string\";\n"
             "/* block: `AlphaFn` at parts/p1.inc */\n")
DATED = ("# Title\n\n> Dated record (S101): code references are as of then; find the code by name with q.\n\nstale `BetaFn` at big.cpp:4\n")


def selftest():
    ok_all = True
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        for d in ("src/x/parts", "docs/ours", "docs/renderer", "tools/t"):
            (root / d).mkdir(parents=True)
        (root / "src/x/big.cpp").write_text(BIG, encoding="utf-8")
        (root / "src/x/parts/p1.inc").write_text("int AlphaFn() {\n  return g_alpha_val;\n}\n", encoding="utf-8")
        (root / "src/x/parts/p2.inc").write_text("int BetaFn() {\n  return 2;\n}\n", encoding="utf-8")
        (root / "src/x/binder.h").write_text("int BinderFn() {\n  return 1;\n}\n", encoding="utf-8")
        (root / "src/x/small.cpp").write_text(SMALL, encoding="utf-8")
        (root / "src/x/s.frag").write_text("void main() {\n  float y = 1.0;\n  float out_colour = y;\n}\n", encoding="utf-8")
        (root / "thirdparty/rexglue-sdk-src/src/graphics").mkdir(parents=True)
        (root / "thirdparty/rexglue-sdk-src/src/graphics/gfx.cpp").write_text("int G() { return 0; }\n", encoding="utf-8")
        files = ["docs/ours/t.md", "src/x/code.h", "docs/ours/dated.md"]

        def go(opts):
            (root / "docs/ours/t.md").write_text(DOC.replace("\n", "\n\n"), encoding="utf-8")   # one paragraph each: no sentence runs into the next line
            (root / "src/x/code.h").write_text(CODE, encoding="utf-8")
            (root / "docs/ours/dated.md").write_text(DATED, encoding="utf-8")
            rep, _ = C.run(root, files, True, opts)
            return [(root / f).read_text(encoding="utf-8") for f in files], rep

        (got_doc, got_code, got_dated), rep = go(C.Opts())
        kinds = {}
        for _r, _o, k, _d in rep:
            kinds[k] = kinds.get(k, 0) + 1
        ok = got_doc == WANT.replace("\n", "\n\n") and got_code == WANT_CODE and got_dated == DATED and kinds.get("gone") == 1
        print("real rules: %s -> %s" % (", ".join("%s %d" % kv for kv in sorted(kinds.items())), "ok" if ok else "WRONG"))
        if not ok:
            print("got :", repr(got_doc))
            print("want:", repr(WANT.replace("\n", "\n\n")))
            print("gotc:", repr(got_code))
        ok_all &= ok
        twins = [("no word check (an enclosing def with no matching word is kept)", C.Opts(word_check=False), 0),
                 ("no size rule (a file size read as a line)", C.Opts(size_rule=False), 0),
                 ("code anywhere (a string literal rewritten)", C.Opts(comments_only=False), 1),
                 ("no name rule (a drifted named ref falls to the enclosing def)", C.Opts(names=False), 0),
                 ("type first (a big enclosing type beats the word on the cited line)", C.Opts(type_last=False), 0),
                 ("no top-level declaration (a table line cites its word, not the table)", C.Opts(top_decl=False), 0),
                 ("no file-only (a drifted / past-the-end ref is left as a line)", C.Opts(file_only=False), 0),
                 ("file-only names the part (the stale line's part, not the parent)", C.Opts(file_parent=False), 0),
                 ("no drop tidy (a dropped follow-on leaves `and` / two spaces)", C.Opts(tidy_drop=False), 0),
                 ("shader main by the declaration path (`(main)` cited)", C.Opts(shader_main=False), 0),
                 ("no bare :N follow-on", C.Opts(follow_on=False), 0),
                 ("dated records read (a `> Dated record` file rewritten)", C.Opts(dated_skip=False), 2)]
        for label, o, idx in twins:
            got, _ = go(o)
            want = [WANT.replace("\n", "\n\n"), WANT_CODE, DATED][idx]
            caught = got[idx] != want
            print("twin %-72s fails on reverted rule: %s" % (label, "yes" if caught else "NO"))
            ok_all &= caught
        # left boundary: the production regex ignores a ref to the file d3_binder.h; an alternation regex without a boundary reads binder.h inside it
        import re
        idx_ = C.Index(root)
        old = re.compile(r"(?P<pre>(?:[A-Za-z0-9_.\-]+[\\/])*)(?P<base>binder\.h):(?P<a>\d+)")
        oldhit = [m.group("base") for m in old.finditer("see d3_binder.h:2 here")]
        c1 = not C.find_refs(idx_, "see d3_binder.h:2 here", C.Opts()) and oldhit == ["binder.h"]
        print("twin left boundary: d3_binder.h not read as binder.h (the boundary-less regex read it: %s): %s" % ("yes" if oldhit else "no", "ok" if c1 else "WRONG"))
        ok_all &= c1
        # an SDK path (thirdparty\rexglue-sdk-src holds it) is not reported as gone; with the rule reverted it is
        _, rep2 = go(C.Opts(sdk_skip=False))
        c3 = sum(1 for x in rep2 if x[2] == "gone") == 2 and kinds.get("gone") == 1
        print("twin sdk path: not gone with the rule, gone without it: %s" % ("ok" if c3 else "WRONG"))
        ok_all &= c3
        # --mark-dated: the opening line, once
        (root / "docs/renderer/m.md").write_text("# M title (Session 101, lane 5)\n\nWritten 2026-11-12 by x.\n", encoding="utf-8")
        C.mark_dated(root, ["docs/renderer/m.md"], True)
        C.mark_dated(root, ["docs/renderer/m.md"], True)
        mk = (root / "docs/renderer/m.md").read_text(encoding="utf-8")
        wantm = ("# M title (Session 101, lane 5)\n\n> Dated record (Session 101, 2026-11-12): code references are as of then; find the code by name with q.\n\n"
                 "Written 2026-11-12 by x.\n")
        c2 = mk == wantm
        print("mark-dated: marker line once, after the title: %s" % ("ok" if c2 else "WRONG %r" % mk))
        ok_all &= c2
    return ok_all
