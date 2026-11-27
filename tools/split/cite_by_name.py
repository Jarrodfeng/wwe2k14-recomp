"""cite_by_name: turn every line-number citation of our code in live docs and code comments into a citation by FILE + NAME.

Rule (CLAUDE.md "Cite our code by name, never by line"; Jarrod 2026-11-27): a live doc or a code comment names the file and the
function / type / cvar / constant / log text, never a line number.  A line number is the fragile part (the SPLIT-DOCS review found
~125 of 206 line refs in live renderer docs already pointing at unrelated lines); the name is what the sentence means.

A ref is `<our file>` + `:N` / `:N-M` / ` line(s) N` / (C-like and shader files only) a bare ` N`, followed by optional `, M` / `/ M`
items, where <our file> is a tracked file under src\\ or tools\\.  Each ref, each follow-on item on its own, ends in one outcome:

  name   the sentence names a definition (backticked or snake_case / CamelCase / Class::Method; function, class, struct, enum)
         that has ONE definition in the cited file's unit (the file plus its split parts): the ref becomes the file where the
         definition lives NOW (a part file for a split unit) and the name, the line dropped.  A name right beside the ref stays
         where it is (`Name` (path)); a name further off is appended: `path (Name)`.  Name search order (the relink rules):
           A  a name BESIDE the ref (before: `Name` (ref, `Name` at ref, `Name`, ref; after: ref (`Name`, ref `Name`, ref = `Name`)
              wins; when the beside names are none of them definitions here (a variable, a cvar, another unit's function) the doc
              attached a non-definition: no name;
           B  else the nearest name BEFORE the ref in its table cell / sentence;
           C  a name AFTER the ref that is not beside it belongs to another clause and is never used.
         Several definitions of the name: the one holding the cited line (through the old line map) if exactly one does.
  encl   no usable name: the definition enclosing the cited line (the split line map for the four split units, the file as it is now
         for the rest) is kept ONLY if a code word of the sentence (a non-plain identifier, a cvar name, quoted log text) occurs
         inside that definition: `path (Enclosing)`.
         A line in NO function: the enclosing top-level declaration (the table / enum / constant / struct / #define it sits in, see
         cite_decls.py), same word check.  A code word ON the cited lines (word) and the enclosing type come after the function.
  file   nothing above fits (the line drifted, the sentence names nothing, the line is past the end): the FILE alone, no number
         (the cited file, the PARENT of a split unit: the stale line was already wrong, so the part it maps to may be too, and q finds
         the code in the parent's unit; the doc's own path when the basename is ambiguous).  A shader's `main` is never a name.
  A name already in the sentence is never repeated in parentheses.  A bare `:N` after a ref is a line of the same file: converted;
  dropped when it adds nothing, with the joiner before it (`REF` and `:M` -> `REF`) or one of the two spaces it leaves.
  gone   a ref written with a src / tools path to a file that no longer exists: listed, left.  A path that is the SDK's
         (thirdparty\\rexglue-sdk-src holds it) is not ours: neither gone nor read.
  DATED docs (the opening line `> Dated record (...)`, put there once by --mark-dated from cite_dated.txt) are not read at all.
A file size is not a citation (rule E): `FILE 7019 -> 409`, `FILE 6033 lines`, `FILE 1024 bytes`, `FILE 2048x1024`, `FILE 2026-11-27`.  A basename shared by several files (two renderer.cpp)
is tried against each candidate; exactly one must pass.  A left boundary keeps `d3_` + `binder.h` from being read as `binder.h`.
Code files: only the text of comments (C-like // and /* */, `#` in .py / .ps1 / .cmake / .sh, and .py docstrings) is read and
rewritten, never code.

  py -I tools\\split\\cite_by_name.py --dry [--files REL ...]    plan + counts, change nothing
  py -I tools\\split\\cite_by_name.py [--files REL ...]          rewrite
  py -I tools\\split\\cite_by_name.py --selftest
Run from the repo root.  Idempotent: a converted tree holds no ref.  The guard that keeps it so: tools\\lint\\cite_line_lint.py.
"""
import ast
import io
import os
import re
import subprocess
import sys
import tokenize
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "q"))
import read_unit as RU  # noqa: E402
import cite_decls  # noqa: E402
import qsym  # noqa: E402

LIVE_GLOBS = ["docs/ours/**/*.md", "docs/systems/**/*.md", "docs/formats/**/*.md", "docs/renderer/**/*.md",
              "PLAYBOOK.md", "STATUS.md", "docs/LESSONS.md", "docs/IDEAS.md", ".claude/agents/*.md"]
SKIP_PARTS = ("history", "archive", "archives")
C_EXT = {".h", ".cpp", ".inc", ".hpp", ".c", ".cc", ".glsl", ".frag", ".vert", ".comp", ".hlsl"}
HASH_EXT = {".ps1", ".cmake", ".sh"}
CODE_EXT = C_EXT | HASH_EXT | {".py"}
SHADER_EXT = {".glsl", ".frag", ".vert", ".comp", ".hlsl"}
BARE_OK = C_EXT   # `FILE 123` (no colon, no "line") is read as a ref only for these: `tool.py 328 329` is a command line

ITEM = re.compile(r"(?P<lead>\s*[/,]\s*:?)(?P<a>\d{3,})(?:[-–](?P<b>\d+))?(?!\d)")
CHAIN = r"~?[A-Za-z_]\w*(?:::~?[A-Za-z_]\w*)*"
TICK_ID = re.compile(r"(?<![\w\\/.])(" + CHAIN + r")(?![\w\\/]|\.\w)")
BARE_ID = re.compile(r"(?<![\w\\/.`])((?:[A-Za-z_]\w*::)+[A-Za-z_]\w*|[A-Za-z]+_\w+|[a-z]+[A-Z]\w*|[A-Z][a-z0-9]+[A-Z]\w*)(?![\w\\/]|\.\w)")
SENT_END = re.compile(r"(?<=[a-z)`\]])[.!?](?=\s)")
NEAR = 150
BESIDE_BEFORE = re.compile(r"[\s`()\[\],:]*(?:(?:at|in|lives in|is at|=)[\s`(\[:]*)?")
BESIDE_AFTER = re.compile(r"[\s`(=]*")
SIZE_AFTER = re.compile(r"\s*(?:->|→|lines?\b|bytes?\b|[KMG]i?B\b|B\b|ms\b|fps\b|%)|[x×]\d")   # a size / count / picture size, not a line
QUICK = re.compile(r"\.[A-Za-z0-9]+(?::\d| lines? \d| \d{3})")   # a file can hold a ref at all (a bare ref has 3+ digits)
DATED_RX = re.compile(r"(?m)^> Dated record \(")
ITEM_START = re.compile(r"\s*(?:[-*+]\s|\d+[.)]\s|#|\|)")


class Opts:
    """The rules, each switchable so the selftest can plant a reverted rule and see its twin fail."""
    def __init__(self, **kw):
        self.left_boundary = True
        self.word_check = True
        self.size_rule = True
        self.comments_only = True
        self.names = True
        self.type_last = True   # a word ON the cited lines beats a big enclosing type that merely holds it
        self.top_decl = True    # a line in no function: the top-level declaration (table / constant / struct) that holds it
        self.file_only = True   # drifted, no usable name, past the end: the file alone, no line number
        self.follow_on = True   # a bare `:N` after a ref is a line of the same file
        self.dated_skip = True  # a doc carrying `> Dated record (...)` is not read
        self.file_parent = True  # file-only names the cited (parent) file, never the part the stale line maps to
        self.tidy_drop = True   # a dropped follow-on takes its joiner (`and`, `,`) or one of its two spaces
        self.shader_main = True  # a shader's main is no name by the top-level declaration path either
        self.sdk_skip = True    # a src / tools path the SDK holds is not reported as gone
        self.__dict__.update(kw)


# ---------------------------------------------------------------------------------------------------------------- file index

def tracked(root):
    root = Path(root)
    if (root / ".git").exists():
        r = subprocess.run(["git", "ls-files", "-z", "--", "src", "tools"], cwd=str(root), capture_output=True)
        if r.returncode == 0:
            return [x for x in r.stdout.decode("utf-8", "replace").split("\0") if x]
    out = []
    for top in ("src", "tools"):
        for dp, _, names in os.walk(str(root / top)):
            for n in names:
                out.append(str(Path(dp, n).relative_to(root)).replace("\\", "/"))
    return out


class Index:
    def __init__(self, root):
        self.root = Path(root)
        self.files = [f for f in tracked(root) if (self.root / f).is_file()]
        self.by = {}
        for f in self.files:
            self.by.setdefault(Path(f).name, []).append(f)
        exts = sorted({Path(f).suffix[1:] for f in self.files if Path(f).suffix}, key=len, reverse=True)
        self.rx = make_regex(exts, True)
        self._sdk = False

    def in_sdk(self, pre, base):
        """True when the SDK checkout (thirdparty\\rexglue-sdk-src, the main checkout's from a worktree) holds pre + base."""
        if self._sdk is False:
            sdk = self.root / "thirdparty" / "rexglue-sdk-src"
            if not sdk.is_dir():
                try:
                    import qx5
                    sdk = qx5.sdk_root(str(self.root))
                except Exception:
                    sdk = None
            self._sdk = Path(sdk) if sdk else None
        return bool(self._sdk) and (self._sdk / (pre.replace("\\", "/") + base)).is_file()

    def candidates(self, pre, base):
        want = pre.replace("\\", "/")
        return [t for t in self.by.get(base, []) if ("/" + t).endswith("/" + want + base)]


def make_regex(exts, left_boundary=True):
    alt = "|".join(re.escape(e) for e in exts)
    return re.compile(
        (r"(?<![\w\-])" if left_boundary else "") +
        r"(?P<pre>(?:[\w.\-]+[\\/])*)(?P<base>[\w.\-]+\.(?:" + alt + r"))"
        r"(?P<sep>:| lines? | (?=\d{3}))(?P<a>\d+)(?:[-–](?P<b>\d+))?(?!\d)"
        r"(?P<tail>(?:\s*[/,]\s*:?\d{3,}(?:[-–]\d+)?(?!\d))*)")


def live_docs(root):
    out = []
    for g in LIVE_GLOBS:
        for p in sorted(Path(root).glob(g)):
            rel = p.relative_to(root).parts
            if p.is_file() and not any(x.lower() in SKIP_PARTS for x in rel) and p not in out:
                out.append(p)
    return out


# ---------------------------------------------------------------------------------------------------------------- comments

def _blank(text, ranges):
    """text with everything outside `ranges` replaced by spaces (newlines / CRs kept)."""
    out = list(re.sub(r"[^\r\n]", " ", text))
    for a, b in ranges:
        out[a:b] = text[a:b]
    return "".join(out)


def c_comment_ranges(text):
    """[(start, end)] of every // and /* */ comment of C-like text (markers included); strings, chars and raw strings skipped."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            while j < n and text[j - 1] == "\\" and j + 1 <= n:   # a continued // comment
                k = text.find("\n", j + 1)
                j = n if k < 0 else k
            out.append((i, j))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append((i, j))
            i = j
        elif c == '"':
            m = re.match(r'R"([^()\\ \t\n]{0,16})\(', text[max(0, i - 1):i + 20]) if i and text[i - 1] == "R" else None
            if m:
                end = text.find(")" + m.group(1) + '"', i)
                i = n if end < 0 else end + len(m.group(1)) + 2
            else:
                i += 1
                while i < n and text[i] != '"' and text[i] != "\n":
                    i += 2 if text[i] == "\\" else 1
                i += 1
        elif c == "'" and not (i and (text[i - 1].isalnum() or text[i - 1] == "_")):
            i += 1
            while i < n and text[i] != "'" and text[i] != "\n":
                i += 2 if text[i] == "\\" else 1
            i += 1
        else:
            i += 1
    return out


def hash_comment_ranges(text, block=False):
    out = []
    pos = 0
    for line in text.split("\n"):
        q = None
        i = 0
        while i < len(line):
            c = line[i]
            if q:
                if c == "`" and q == '"':
                    i += 1
                elif c == q:
                    q = None
            elif c in "'\"":
                q = c
            elif c == "#":
                out.append((pos + i, pos + len(line)))
                break
            i += 1
        pos += len(line) + 1
    if block:
        for m in re.finditer(r"<#.*?#>", text, re.S):
            out.append((m.start(), m.end()))
    return out


def py_comment_ranges(text):
    """comments and statement-level strings (docstrings) of Python text; [] when it does not tokenize."""
    starts = [0]
    for m in re.finditer("\n", text):
        starts.append(m.end())
    def off(rc):
        return starts[rc[0] - 1] + rc[1]
    out = []
    prev = None
    toks = []
    try:
        for t in tokenize.generate_tokens(io.StringIO(text).readline):
            toks.append(t)
    except (tokenize.TokenError, IndentationError, SyntaxError):
        return None
    skip = (tokenize.NL, tokenize.COMMENT)
    sig = [t for t in toks if t.type not in skip]
    for t in toks:
        if t.type == tokenize.COMMENT:
            out.append((off(t.start), off(t.end)))
    for k, t in enumerate(sig):
        if t.type == tokenize.STRING:
            before = sig[k - 1].type if k else tokenize.NEWLINE
            after = sig[k + 1].type if k + 1 < len(sig) else tokenize.ENDMARKER
            if before in (tokenize.NEWLINE, tokenize.INDENT, tokenize.DEDENT, tokenize.ENCODING) and after in (tokenize.NEWLINE, tokenize.ENDMARKER):
                out.append((off(t.start), off(t.end)))
    return out


def comment_view(rel, text, opts=None):
    """The text as the scanner reads it: a doc as it is; a code file with everything outside comments blanked (offsets unchanged),
    comment markers blanked too.  None for a file kind that has no comments we read."""
    ext = Path(rel).suffix.lower()
    if ext == ".md":
        return text
    if ext in C_EXT:
        rg = c_comment_ranges(text)
    elif ext == ".py":
        rg = py_comment_ranges(text)
        if rg is None:
            return None
    elif ext in HASH_EXT:
        rg = hash_comment_ranges(text, ext == ".ps1")
    else:
        return None
    view = _blank(text, rg)
    if ext in C_EXT:
        view = re.sub(r"//|/\*|\*/", "  ", view)
    elif ext in HASH_EXT:
        view = view.replace("#", " ")
    return view


# ---------------------------------------------------------------------------------------------------------------- definitions

def py_spans(path):
    try:
        tree = ast.parse(Path(path).read_text(encoding="utf-8", errors="replace"))
    except (SyntaxError, ValueError):
        return []
    out = []

    def walk(node, chain):
        for ch in ast.iter_child_nodes(node):
            if isinstance(ch, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                qual = ".".join(chain + [ch.name])
                kind = "type" if isinstance(ch, ast.ClassDef) else "func"
                out.append({"kind": kind, "name": ch.name, "qual": qual, "start": ch.lineno, "end": ch.end_lineno or ch.lineno})
                walk(ch, chain + [ch.name])
            else:
                walk(ch, chain)
    walk(tree, [])
    return out


def ps1_spans(path):
    lines = Path(path).read_text(encoding="utf-8", errors="replace").split("\n")
    heads = [(i + 1, m.group(1)) for i, ln in enumerate(lines) for m in [re.match(r"\s*function\s+([\w\-:]+)", ln, re.I)] if m]
    out = []
    for k, (ln, name) in enumerate(heads):
        end = heads[k + 1][0] - 1 if k + 1 < len(heads) else len(lines)
        out.append({"kind": "func", "name": name, "qual": name, "start": ln, "end": end})
    return out


class Mapper:
    """Per cited file: its unit (the file plus split parts), line map, definitions, enclosing definition."""
    def __init__(self, root):
        self.root = Path(root)
        self.maps, self.index, self.srcs, self.spanc, self.decls = {}, {}, {}, {}, {}

    def lmap(self, rel):
        if rel not in self.maps:
            if Path(rel).suffix.lower() in C_EXT:
                self.maps[rel] = RU.line_map(self.root / rel)
            else:
                p = self.root / rel
                n = len(p.read_bytes().split(b"\n"))
                self.maps[rel] = [(p, i) for i in range(1, n + 1)]
        return self.maps[rel]

    def file_spans(self, f):
        f = Path(f).resolve()
        if f not in self.spanc:
            ext = f.suffix.lower()
            if ext in C_EXT:
                rel = f.relative_to(self.root.resolve()).as_posix()
                sp = [s for s in qsym.Src(str(self.root.resolve()), rel).spans if s["kind"] in ("func", "type", "enum") and s["name"] and s["end"] > s["start"]]
            elif ext == ".py":
                sp = py_spans(f)
            elif ext == ".ps1":
                sp = ps1_spans(f)
            else:
                sp = []
            self.spanc[f] = sp
        return self.spanc[f]

    def src_lines(self, f):
        f = Path(f).resolve()
        if f not in self.srcs:
            self.srcs[f] = f.read_text(encoding="utf-8", errors="replace").split("\n")
        return self.srcs[f]

    def defs(self, rel, chain):
        """[(Path file, start, end, qual)] of the name in rel's unit (functions and types)."""
        if rel not in self.index:
            idx = self.index[rel] = {}
            for f in sorted({f.resolve() for f, _ in self.lmap(rel)}):
                for sp in self.file_spans(f):
                    tail = re.split(r"::|\.", sp["name"])[-1].lstrip("~")
                    idx.setdefault(tail, []).append((f, sp["start"], sp["end"], sp.get("qual") or sp["name"]))
        parts = chain.split("::")
        ent = self.index[rel].get(parts[-1].lstrip("~"), [])
        if len(parts) > 1:
            ent = [e for e in ent if parts[-2] in re.split(r"::|\.", e[3])]
        return ent

    def enclosing(self, rel, line):
        """(file, span) of the innermost definition holding expanded line `line` of rel's unit, else None."""
        m = self.lmap(rel)
        if not 1 <= line <= len(m):
            return None
        f, n = m[line - 1]
        best = None
        for sp in self.file_spans(f):
            if sp["start"] <= n <= sp["end"] and (best is None or sp["start"] >= best["start"]):
                best = sp
        return (f.resolve(), best) if best else None

    def top_decl(self, f, n):
        """(start, end, name) of the file-scope declaration holding line n of file f (a #define, a table, a constant, a struct), else None."""
        f = Path(f).resolve()
        if f not in self.decls:
            ext = f.suffix.lower()
            if ext in C_EXT:
                rel = f.relative_to(self.root.resolve()).as_posix()
                src = qsym.Src(str(self.root.resolve()), rel)
                self.decls[f] = cite_decls.c_top_decls(src.text, src.masked)
            elif ext == ".py":
                self.decls[f] = cite_decls.py_top_decls(f)
            else:
                self.decls[f] = []
        if f.suffix.lower() in C_EXT:
            d = cite_decls.define_at(self.src_lines(f), n)
            if d:
                return d
        best = None
        for s, e, name in self.decls[f]:
            if s <= n <= e and (best is None or s >= best[0]):
                best = (s, e, name)
        return best

    @staticmethod
    def weak(chain):
        tail = re.split(r"::|\.", chain)[-1].lstrip("~")
        return "::" not in chain and "_" not in tail and sum(c.isupper() for c in tail) < 2


# ---------------------------------------------------------------------------------------------------------------- context

def paragraph(text, s, e):
    """(start, end) of the paragraph holding text[s:e]: a table row alone, else the run of non-blank lines up to a list item /
    heading / table row."""
    ls = text.rfind("\n", 0, s) + 1
    le = text.find("\n", e)
    le = len(text) if le < 0 else le
    if text[ls:le].lstrip().startswith("|"):
        return ls, le
    ps, pe = ls, le
    while ps > 0 and not ITEM_START.match(text, ps):
        pl = text.rfind("\n", 0, ps - 1) + 1
        prev = text[pl:ps - 1]
        if not prev.strip() or prev.lstrip().startswith("|"):
            break
        ps = pl
    while pe < len(text):
        nl = text.find("\n", pe + 1)
        nl = len(text) if nl < 0 else nl
        nxt = text[pe + 1:nl]
        if not nxt.strip() or ITEM_START.match(nxt):
            break
        pe = nl
    return ps, pe


def context_names(text, s, e, others=(), near=NEAR, wide=False):
    """Identifier chains next to the ref text[s:e], nearest first: [(chain, distance, side, beside)]. Scope: table cell, else
    the sentence of the paragraph."""
    ps, pe = paragraph(text, s, e)
    line = text[ps:pe]
    lo, hi = s - ps, e - ps
    if wide:
        a, b = 0, len(line)   # the whole table row / paragraph
    elif line.lstrip().startswith("|"):
        a = line.rfind("|", 0, lo) + 1
        b = line.find("|", hi)
        b = len(line) if b < 0 else b
    else:
        a = 0
        for m in SENT_END.finditer(line, 0, lo):
            a = m.end()
        m = SENT_END.search(line, hi)
        b = m.start() if m else len(line)
    cands, ticked = [], []
    for tm in re.finditer(r"`([^`\n]*)`", line):
        if tm.start() < a or tm.end() > b:
            continue
        ticked.append((tm.start(), tm.end()))
        for im in TICK_ID.finditer(tm.group(1)):
            cands.append((tm.start(1) + im.start(1), tm.start(1) + im.end(1), im.group(1), 0))
    for im in BARE_ID.finditer(line, a, b):
        if any(x <= im.start(1) < y for x, y in ticked):
            continue
        cands.append((im.start(1), im.end(1), im.group(1), 1))
    out = []
    for cs, ce, name, bare in cands:
        if ce > lo and cs < hi:
            continue
        d = lo - ce if ce <= lo else cs - hi + 60
        if any((ce + ps <= os_ and oe <= s) or (e <= os_ and oe <= cs + ps) for os_, oe in others):
            continue
        gap = line[ce:lo] if ce <= lo else line[hi:cs]
        beside = len(gap) <= 12 and bool((BESIDE_BEFORE if ce <= lo else BESIDE_AFTER).fullmatch(gap))
        if d <= near and len(name.lstrip("~")) >= 3:
            out.append((d, bare, name, "b" if ce <= lo else "a", beside))
    out.sort()
    return [(n, d, side, bes) for d, _, n, side, bes in out], line[a:b]


# ---------------------------------------------------------------------------------------------------------------- the work

class Ref:
    __slots__ = ("start", "end", "head_end", "pre", "base", "sep", "a", "b", "tail", "items", "cands", "bare")


FOLLOW = re.compile(r"(?P<lead>[^:\n|]{0,40}?)(?<![\w:])(?P<c>:)(?P<a>\d{2,})(?:[-–](?P<b>\d+))?(?![\w:]|\d)")


def find_refs(index, view, opts, rx=None, gone=None):
    """[Ref] of every citation of one of our files in `view`; each with candidates resolved later.  `gone` (a list) collects the
    refs written with a src / tools path to a file that no longer exists."""
    out = []
    rx = rx or index.rx
    for m in rx.finditer(view):
        base = m.group("base")
        cands = index.candidates(m.group("pre"), base) if base in index.by else []
        if not cands:
            if gone is not None and m.group("pre").replace("\\", "/").startswith(("src/", "tools/")) \
                    and not (opts.sdk_skip and index.in_sdk(m.group("pre"), base)):
                gone.append(m.group("pre") + base + m.group("sep") + m.group("a"))
            continue
        sep = m.group("sep")
        if sep == " " and "." + base.rsplit(".", 1)[-1].lower() not in BARE_OK:
            continue   # `tool.py 328 329` is a command line
        if sep == " " and not m.group("tail") and opts.size_rule and SIZE_AFTER.match(view, m.end()):
            continue
        if sep == " " and opts.size_rule and m.group("b") and (int(m.group("b")) < int(m.group("a")) or view[m.end():m.end() + 1] in "-–"):
            continue   # `FILE 2026-11-27`: a date, not a line range
        r = Ref()
        r.start, r.end = m.start(), m.end()
        r.head_end = m.start("tail") if m.group("tail") else m.end()
        r.pre, r.base, r.sep = m.group("pre"), base, sep
        r.a = int(m.group("a"))
        r.b = int(m.group("b")) if m.group("b") else r.a
        r.tail = m.group("tail") or ""
        r.items = []
        pos = 0
        while pos < len(r.tail):
            im = ITEM.match(r.tail, pos)
            if not im:
                break
            ta = int(im.group("a"))
            tb = int(im.group("b")) if im.group("b") else ta
            at = m.start("tail")
            r.items.append((at + im.start("lead"), at + im.start("a"), at + im.end(), ta, tb, im.group("lead")))
            pos = im.end()
        r.bare = []
        pos = m.end()
        while opts.follow_on and len(r.bare) < 8:   # `FILE:N (what), `:M` (what)`: a bare :M after the ref is a line of the same file
            fm = FOLLOW.match(view, pos)
            if not fm or index.rx.search(fm.group("lead")) or SENT_END.search(fm.group("lead")):
                break
            r.bare.append((fm.start("c"), fm.end(), int(fm.group("a")), int(fm.group("b")) if fm.group("b") else int(fm.group("a")), fm.start()))
            pos = fm.end()
        r.cands = cands
        out.append(r)
    return out


class Worker:
    def __init__(self, root, opts=None):
        self.root = Path(root)
        self.opts = opts or Opts()
        self.index = Index(root)
        self.mapper = Mapper(root)

    # ---- one cited number against one candidate file
    def resolve_name(self, rel, view, s, e, others):
        names, _sent = context_names(view, s, e, others)
        mp = self.mapper
        beside = [c for c, _d, _side, bes in names if bes]
        for chain in beside:
            d = mp.defs(rel, chain) if not mp.weak(chain) else []
            if d:
                return chain, d, True
        if beside:
            return None
        for chain, _d, side, _b in names:
            if side != "b" or mp.weak(chain):
                continue
            d = mp.defs(rel, chain)
            if d:
                return chain, d, False
        return None

    def code_words(self, view, s, e, others):
        names, sent = context_names(view, s, e, others, near=10 ** 6)
        words = {n.split("::")[-1].lstrip("~") for n, _d, _s, _b in names if not self.mapper.weak(n) and len(n.lstrip("~")) >= 4}
        quotes = [q for q in re.findall(r'"([^"\n]{8,})"', sent)]
        quotes += [q for q in re.findall(r"`([^`\n]{8,})`", sent) if " " in q]
        return words, quotes

    def eval_item(self, rel, view, s, e, a, b, others):
        """('name', file, None) | ('encl', file, label) | ('word', file, hit) | ('file', file, None) | ('hand', why).
        The order: a name of the sentence; the enclosing function (a code word of the sentence inside it); for a line in no
        function the enclosing top-level declaration (same word check); a code word ON the cited lines; the enclosing type
        (word inside); else the FILE alone (the part file the line maps to, the cited file when it maps nowhere)."""
        mp = self.mapper
        m = mp.lmap(rel)
        here = m[a - 1] if 1 <= a <= len(m) else None
        if self.opts.names:
            sym = self.resolve_name(rel, view, s, e, others)
            if sym:
                chain, defs, _beside = sym
                if len(defs) > 1 and here:
                    defs = [d for d in defs if d[0].resolve() == here[0].resolve() and d[1] <= here[1] <= d[2]]
                if len(defs) == 1:
                    return ("name", defs[0][0], None)
        parent = (self.root / rel).resolve()
        if here is None:
            return ("file", parent, None) if self.opts.file_only else ("hand", "line %d is past the end of the file (%d lines)" % (a, len(m)))
        words, quotes = self.code_words(view, s, e, others)

        def find_hit(text):
            h = next((w for w in sorted(words) if re.search(r"(?<![\w])" + re.escape(w) + r"(?![\w])", text)), None)
            return h or next((q for q in quotes if q in text), None)

        def label(sp):
            return re.sub(r"[A-Z][A-Z0-9_]*\((\w+)\)$", r"\1", sp.get("qual") or sp["name"])   # REX_HOOK_RAW(sub_X) -> sub_X
        enc = mp.enclosing(rel, a)
        if enc and enc[1]["name"] == "main" and Path(enc[0]).suffix.lower() in SHADER_EXT:
            enc = None   # a shader's entry point is no name to cite by
        in_type = False
        if enc:
            f, sp = enc
            in_type = sp["kind"] != "func"
            text = "\n".join(mp.src_lines(f)[sp["start"] - 1:sp["end"]])
            if not self.opts.word_check or ((not in_type or not self.opts.type_last) and find_hit(text)):
                return ("encl", f, label(sp))
        elif self.opts.top_decl:
            td = mp.top_decl(here[0], here[1])
            if td and td[2] == "main" and self.opts.shader_main and Path(here[0]).suffix.lower() in SHADER_EXT:
                td = None   # the shader's entry point again, by the declaration path
            if td and (not self.opts.word_check or find_hit("\n".join(mp.src_lines(here[0])[td[0] - 1:td[1]]))):
                return ("encl", here[0].resolve(), td[2])
        # a code word of the sentence ON the cited lines names the place
        lines = []
        for ln in range(a, min(b, len(m)) + 1):
            f2, n2 = m[ln - 1]
            lines.append(mp.src_lines(f2)[n2 - 1] if n2 - 1 < len(mp.src_lines(f2)) else "")
        hit = find_hit("\n".join(lines)) if self.opts.word_check else None
        if hit:
            return ("word", m[a - 1][0].resolve(), hit)
        if enc and in_type and self.opts.type_last and find_hit(text):
            return ("encl", f, label(sp))   # inside a type that holds the word, but not on the cited lines
        if self.opts.file_only:   # drifted, no usable name: the number was wrong, so the part it maps to may be wrong too: the parent
            return ("file", parent if self.opts.file_parent else here[0].resolve(), None)
        return ("hand", "line %d is in %s, which holds none of the sentence's code words" % (a, label(enc[1]) if enc else "no function / type"))

    def fmt_path(self, rel, pre, base, f):
        f = Path(f).resolve()
        parent = (self.root / rel).resolve()
        if f == parent:
            return pre + base
        try:
            name = pre + f.relative_to(parent.parent).as_posix()
        except ValueError:
            name = f.relative_to(self.root.resolve()).as_posix()
        if "\\" in pre:
            name = name.replace("/", "\\")
        return name

    def sentence_of(self, view, s, e):
        return context_names(view, s, e, self._others, near=10 ** 6)[1]

    def cite_text(self, rel, r, it, view, s, e):
        """The text a ref (or one follow-on item) becomes.  A name that is already in the sentence is never repeated in parentheses."""
        path = self.fmt_path(rel, r.pre, r.base, it[1])
        if it[0] in ("name", "word", "file") or it[2] is None:
            return path
        tail = re.split(r"::|\.", it[2])[-1]
        if tail and re.search(r"(?<![\w])" + re.escape(tail) + r"(?![\w])", self.sentence_of(view, s, e)):
            return path
        return "%s (%s)" % (path, it[2])

    def convert_ref(self, r, view):
        """([(start, end, new)], [(item text, kind, detail)]) for one ref: kind name / encl / word / file / drop."""
        others = self._others
        results = []   # a basename shared by several files: each candidate is tried
        spans_in = [(r.start, r.head_end)] + [(ia, ie) for (_ls, ia, ie, _ta, _tb, _lead) in r.items] + [(cs, ce) for (cs, ce, _a, _b, _l) in r.bare]
        nums = [(r.a, r.b)] + [(ta, tb) for (_ls, _ia, _ie, ta, tb, _lead) in r.items] + [(a, b) for (_cs, _ce, a, b, _l) in r.bare]
        for rel in r.cands:
            results.append((rel, [self.eval_item(rel, view, s, e, a, b, others) for (s, e), (a, b) in zip(spans_in, nums)]))
        sure = [(rel, res) for rel, res in results if res[0][0] in ("name", "encl", "word")]
        ambiguous = False
        if len(r.cands) > 1:
            if len(sure) != 1:
                ambiguous = True   # which of the files is meant is not settled: the doc's own path stays, the number goes
            else:
                rel, res = sure[0]
        if not ambiguous:
            if len(r.cands) == 1:
                rel, res = results[0]
            res = list(res)
        else:
            rel = r.cands[0]
            res = [("file", None, None)] * len(nums)
        nitems = len(r.items)
        edits, report = [], []
        head_orig = r.pre + r.base + r.sep + str(r.a) + ("-%d" % r.b if r.b != r.a else "")

        def new_for(k):
            it = res[k]
            if it[0] == "hand":
                return None
            if ambiguous:
                return r.pre + r.base
            return self.cite_text(rel, r, it, view, *spans_in[k])
        h = new_for(0)
        if h is None:
            report.append((head_orig, "hand", res[0][1]))
        else:
            edits.append((r.start, r.head_end, h))
            report.append((head_orig, "file" if res[0][0] == "file" else res[0][0], h))
        last = h
        for k, (ls, ia, ie, ta, tb, lead) in enumerate(r.items, 1):
            orig = r.pre + r.base + ":" + str(ta) + ("-%d" % tb if tb != ta else "")
            n = new_for(k)
            if n is None:
                report.append((orig, "hand", res[k][1]))
                last = None
                continue
            if n == last:   # the same cite again: its lead and item go
                edits.append((ls, ie, ""))
                report.append((orig, "drop", "same cite again"))
            else:
                ld = lead.rstrip(":")
                if self.opts.tidy_drop and ld.strip() == "," and not ld[-1:].isspace():
                    ld += " "   # `FILE:10,20` -> `FILE (A), FILE (B)`
                edits.append((ls, ie, ld + n))
                report.append((orig, res[k][0], n))
            last = n
        for j, (cs, ce, a, b, ls) in enumerate(r.bare):
            k = 1 + nitems + j
            n = new_for(k)
            orig = ":" + str(a) + ("-%d" % b if b != a else "")
            if n is None:
                report.append((orig, "hand", res[k][1]))
                continue
            if n == h or ambiguous:   # no new information: the number goes (with the backticks that held only it)
                s0, e0 = cs, ce
                if s0 > 0 and ce < len(view) and view[s0 - 1] == "`" and view[ce] == "`":
                    s0, e0 = s0 - 1, ce + 1
                    m2 = re.search(r"(?:,|;)\s*$", view[max(0, s0 - 6):s0])
                    if m2 and not self.opts.tidy_drop:
                        s0 -= len(m2.group(0))
                if self.opts.tidy_drop:
                    jm = re.fullmatch(r"(`?)\s*(?:[,;/]|and|or)?\s*", view[ls:s0])
                    if view[s0 - 1:s0] == "(" and view[e0:e0 + 1] == ")":   # `the hold (:M)` -> `the hold`: no empty parentheses
                        s0, e0 = s0 - 1 - (view[s0 - 2:s0 - 1] == " "), e0 + 1
                    elif jm:   # `REF` and `:M` -> `REF`: only a joiner between them, it goes with the number
                        s0 = ls + len(jm.group(1))
                    elif view[s0 - 1:s0] == " " and (e0 >= len(view) or view[e0] in " ,;.)\n\r"):
                        s0 -= 1   # `A, :M B` -> `A, B`: one space stays, not two
                edits.append((s0, e0, ""))
                report.append((orig, "drop", "follow-on number: the cite above is the same place"))
            else:
                edits.append((cs, ce, n))
                report.append((orig, res[k][0], n))
        return edits, report

    # ---- one file
    def run_file(self, rel, text):
        """(new text, [(file, ref text, kind, detail)])"""
        if self.opts.dated_skip and rel.endswith(".md") and is_dated(text):
            return text, []
        view = comment_view(rel, text, self.opts) if self.opts.comments_only or Path(rel).suffix == ".md" else text
        if view is None:
            return text, []
        gone = []
        self._all = find_refs(self.index, view, self.opts, gone=gone)
        report = [(rel, g, "gone", "no such file") for g in gone]
        if not self._all:
            return text, report
        self._others = [(x.start, x.head_end) for x in self._all] + [(i[1], i[2]) for x in self._all for i in x.items]
        edits = []
        for r in self._all:
            ed, rep = self.convert_ref(r, view)
            edits += ed
            report += [(rel, o, k, d) for o, k, d in rep]
        edits.sort()
        out, pos = [], 0
        for st, en, new in edits:
            if st < pos:
                continue
            out.append(text[pos:st])
            out.append(new)
            pos = en
        out.append(text[pos:])
        return "".join(out), report


def scan_files(root, index):
    """Repo-relative posix paths of every file in scope: live docs + code files of src / tools."""
    out = [p.relative_to(root).as_posix() for p in live_docs(root)]
    out += [f for f in index.files if Path(f).suffix.lower() in CODE_EXT]
    return out


def run(root, files, apply, opts=None):
    w = Worker(root, opts)
    if files is None:
        files = scan_files(Path(root), w.index)
    rep, changed = [], 0
    for rel in files:
        p = Path(root) / rel
        if not p.is_file():
            continue
        raw = p.read_bytes().decode("utf-8", errors="surrogateescape")   # a file with a stray non-UTF-8 byte keeps it
        if not QUICK.search(raw):
            continue
        new, r = w.run_file(rel, raw)
        rep += r
        if new != raw:
            changed += 1
            if apply:
                p.write_bytes(new.encode("utf-8", errors="surrogateescape"))
    return rep, changed


def is_dated(text):
    """A doc that carries the opening line `> Dated record (...)`: its code references are a snapshot, the tool and the lint skip it."""
    return bool(DATED_RX.search(text[:6000]))


def dated_label(text):
    head = "\n".join(text.split("\n")[:8])
    parts = []
    m = re.search(r"Session \d+|\bS\d{2,3}\b", head)
    if m:
        parts.append(m.group(0))
    m = re.search(r"\d{4}-\d\d-\d\d", head)
    if m:
        parts.append(m.group(0))
    return ", ".join(parts) or "undated"


def mark_dated(root, rels, apply):
    """Give each doc `rels` the opening line  > Dated record (<session or date>): ...  after its title, unless it has one. [rel marked]."""
    done = []
    for rel in rels:
        p = Path(root) / rel
        raw = p.read_bytes().decode("utf-8", errors="surrogateescape")
        if is_dated(raw):
            continue
        nlc = "\r\n" if "\r\n" in raw else "\n"
        lines = raw.split(nlc)
        marker = "> Dated record (%s): code references are as of then; find the code by name with q." % dated_label(raw)
        i = next((k for k, ln in enumerate(lines) if ln.startswith("# ")), None)
        if i is None:
            new = [marker, ""] + lines
        elif i + 1 < len(lines) and not lines[i + 1].strip():
            new = lines[:i + 2] + [marker, ""] + lines[i + 2:]
        else:
            new = lines[:i + 1] + ["", marker, ""] + lines[i + 1:]
        done.append(rel)
        if apply:
            p.write_bytes(nlc.join(new).encode("utf-8", errors="surrogateescape"))
    return done


def list_refs(root, files=None, opts=None):
    """[(file, ref text)] of every line citation in scope (no resolution): the lint's input.  ref text as `run` reports it."""
    opts = opts or Opts()
    idx = Index(root)
    out = []
    for rel in (files if files is not None else scan_files(Path(root), idx)):
        p = Path(root) / rel
        if not p.is_file():
            continue
        raw = p.read_bytes().decode("utf-8", errors="replace")
        if not QUICK.search(raw):
            continue
        if opts.dated_skip and p.suffix == ".md" and is_dated(raw):
            continue
        view = comment_view(rel, raw) if opts.comments_only or p.suffix == ".md" else raw
        if view is None:
            continue
        for r in find_refs(idx, view, opts):
            out.append((rel, r.pre + r.base + r.sep + str(r.a) + ("-%d" % r.b if r.b != r.a else "")))
            for (_ls, _ia, _ie, ta, tb, _lead) in r.items:
                out.append((rel, r.pre + r.base + ":" + str(ta) + ("-%d" % tb if tb != ta else "")))
    return out


def summarize(rep, apply, changed):
    kinds = {}
    per = {}
    for rel, o, k, d in rep:
        kinds[k] = kinds.get(k, 0) + 1
        g = per.setdefault(rel, {})
        g[k] = g.get(k, 0) + 1
    cols = ("name", "encl", "word", "file", "drop", "hand", "gone")
    print("%-52s %s" % ("file", " ".join("%5s" % c for c in cols)))
    for rel in sorted(per):
        print("%-52s %s" % (rel, " ".join("%5d" % per[rel].get(c, 0) for c in cols)))
    print("%s: %s (%d files %s)" % ("APPLIED" if apply else "dry", ", ".join("%s %d" % (c, kinds.get(c, 0)) for c in cols), changed,
                                    "changed" if apply else "would change"))


def main(argv):
    if "--selftest" in argv:
        import cite_selftest
        ok = cite_selftest.selftest()
        print("selftest " + ("PASS" if ok else "FAIL"))
        return 0 if ok else 1
    root = Path(".").resolve()
    if "--mark-dated" in argv:   # tools\split\cite_dated.txt: `path | DATED / LIVE | reason`; each DATED doc gets the opening line once
        rels = [ln.split(" | ")[0].strip() for ln in (HERE / "cite_dated.txt").read_text(encoding="utf-8").splitlines()
                if " | DATED | " in ln]
        done = mark_dated(root, rels, "--dry" not in argv)
        print("%s: %d of %d DATED docs given the opening line" % ("dry" if "--dry" in argv else "marked", len(done), len(rels)))
        return 0
    files = None
    if "--files" in argv:
        files = [a.replace("\\", "/") for a in argv[argv.index("--files") + 1:] if not a.startswith("--")]
    apply = "--dry" not in argv
    rep, changed = run(root, files, apply)
    if "--hand" in argv:   # only the unsettled refs, grouped by doc, each with its sentence
        cur = None
        for rel, o, k, d in sorted(x for x in rep if x[2] in ("hand", "gone")):
            if rel != cur:
                print("\n== %s" % rel)
                cur = rel
            print("  %s  [%s]" % (o, d))
        print("\nhand: %d refs in %d files" % (sum(1 for x in rep if x[2] == "hand"), len({x[0] for x in rep if x[2] == "hand"})))
        return 0
    for rel, o, k, d in rep:
        if k in ("hand", "gone"):
            print("  %s  %s: %s -> %s" % (k.upper(), rel, o, d))
        elif "--verbose" in argv or not apply:
            print("  %-5s %s: %s -> %s" % (k, rel, o, d))
    summarize(rep, apply, changed)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
