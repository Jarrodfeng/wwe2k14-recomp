"""read_unit: a source file with its split parts expanded back in place.

A split (docs/research/s111/SPLITPLAN.md) moves whole runs of a big file into part files and leaves, at the exact
spot, one line  #include "dir/part.inc"  // split: part
This helper puts the part text back, so a tool that reads the parent by text sees the old file with the old line
numbers.  Only include lines that carry the `// split: NAME` marker are expanded (recursively); every other
#include stays as it is.

  from tools.split.read_unit import read_unit      (or put tools/split on sys.path)
  text = read_unit(root, "src/launcher/launcher_main.cpp")
  py -I tools\\split\\read_unit.py FILE      prints the expanded text to stdout
"""
import os
import re
import sys
from pathlib import Path

MARK = re.compile(rb'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"[ \t]*//[ \t]*split:[ \t]*(\S+)[ \t]*\r?\n?$')


def expand_bytes(path, _depth=0):
    """Bytes of `path` with every marked include line replaced by that part's (expanded) bytes."""
    path = Path(path)
    if _depth > 8:
        raise RuntimeError("split include nesting too deep at " + str(path))
    out = []
    for line in path.read_bytes().splitlines(keepends=True):
        m = MARK.match(line)
        if not m:
            out.append(line)
            continue
        part = path.parent / m.group(1).decode("utf-8")
        if not part.is_file():
            raise FileNotFoundError("split part missing: %s (named in %s)" % (part, path))
        out.append(expand_bytes(part, _depth + 1))
    return b"".join(out)


def read_text(path, errors="replace"):
    """`path` with its parts expanded, as text the way a text-mode open() / Path.read_text() gives it: UTF-8, every line ending
    (CRLF, a lone CR) turned into '\\n'. expand_bytes keeps the checkout's CRLF (core.autocrlf), so a tool that read the parent in
    text mode before the split and switched to expand_bytes().decode() sees '\\r\\n': its '\\n' anchors and `$` regexes stop
    matching without an error (the SPLIT-LAUNCHER review: 4 of ramp_menu_selftest's planted twins lost their anchors)."""
    return expand_bytes(path).decode("utf-8", errors=errors).replace("\r\n", "\n").replace("\r", "\n")


def read_unit(root, rel):
    """The parent `rel` (under `root`) as text (read_text), parts expanded, line numbers equal to the pre-split file's."""
    return read_text(Path(root) / rel)


def read_path(path, crlf=False):
    """Like open(path, encoding='utf-8', errors='replace').read() but parts expanded; line endings as the default
    text mode gives them (CRLF -> LF) unless crlf=True (as open(..., newline='') gives)."""
    if not crlf:
        return read_text(path)   # text mode: CRLF and lone CR become '\n'
    return expand_bytes(path).decode("utf-8", errors="replace")   # newline='': the checkout's line endings kept


def part_files(src_dir):
    """Resolved Paths of every part file (marked include target) under `src_dir`: a scanner that walks a tree reads each
    parent with read_unit / read_path and must then skip these, or it sees their text twice."""
    out = set()
    for dp, _, names in os.walk(str(src_dir)):
        for n in names:
            if not n.endswith((".h", ".cpp", ".inc", ".hpp", ".c")):
                continue
            p = Path(dp) / n
            try:
                data = p.read_bytes()
            except OSError:
                continue
            if b"split:" not in data:
                continue
            for line in data.splitlines():
                m = MARK.match(line)
                if m:
                    out.add((p.parent / m.group(1).decode("utf-8")).resolve())
    return out


_PART_EXTS = (".h", ".cpp", ".inc", ".hpp", ".c")
_DIR_MARKS = {}   # folder (str) -> [(includer Path, 1-based line, resolved target Path)]: the marked include lines of the files directly in it


def _dir_marks(d):
    key = str(d)
    if key not in _DIR_MARKS:
        out = []
        try:
            names = sorted(os.listdir(key))
        except OSError:
            names = []
        for n in names:
            if not n.endswith(_PART_EXTS):
                continue
            p = Path(d) / n
            try:
                data = p.read_bytes()
            except OSError:
                continue
            if b"split:" not in data:
                continue
            for i, line in enumerate(data.splitlines(), 1):
                m = MARK.match(line)
                if m:
                    out.append((p, i, (p.parent / m.group(1).decode("utf-8")).resolve()))
        _DIR_MARKS[key] = out
    return _DIR_MARKS[key]


def includer_of(path, levels=3):
    """(includer Path, 1-based line of its `// split:` include line) of part file `path`; None when no file in path's folder or the
    `levels` - 1 folders above it pulls `path` in through a marked include (any file that is not a split part, e.g. an unmarked .inc)."""
    path = Path(path).resolve()
    d = path.parent
    for _ in range(levels):
        for inc, line, target in _dir_marks(d):
            if target == path:
                return inc, line
        if d.parent == d:
            break
        d = d.parent
    return None


def unit_of(path):
    """The split parent a part belongs to (its includer's includer ..., the file the build includes): reader/registry.inc -> reader.h.
    `path` itself (resolved) when it is no part. A tool that keys anything by file name keys a part by this."""
    p = Path(path).resolve()
    seen = set()
    while p not in seen:
        seen.add(p)
        inc = includer_of(p)
        if inc is None:
            return p
        p = inc[0].resolve()
    return p


def line_map(path, _depth=0):
    """[(file, line_no)] for every line of the expanded `path`, in order (file is a Path, line_no 1-based)."""
    path = Path(path)
    if _depth > 8:
        raise RuntimeError("split include nesting too deep at " + str(path))
    out = []
    for n, line in enumerate(path.read_bytes().splitlines(keepends=True), 1):
        m = MARK.match(line)
        if not m:
            out.append((path, n))
            continue
        out.extend(line_map(path.parent / m.group(1).decode("utf-8"), _depth + 1))
    return out


def locate(root, rel, line):
    """Where expanded line `line` (1-based, as read_unit counts it) of parent `rel` lives: 'rel/of/part.inc:LINE'
    (relative to `root`, forward slashes); the parent's own 'rel:LINE' when it is not in a part."""
    m = line_map(Path(root) / rel)
    if not 1 <= line <= len(m):
        return "%s:%d" % (rel, line)
    f, n = m[line - 1]
    try:
        f = f.resolve().relative_to(Path(root).resolve())
    except ValueError:
        pass
    return "%s:%d" % (str(f).replace("\\", "/"), n)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--locate":
        sys.exit(print(locate(".", sys.argv[2], int(sys.argv[3]))))
    if len(sys.argv) != 2:
        sys.exit("usage: read_unit.py FILE   |   read_unit.py --locate FILE LINE   (run from the repo root)")
    sys.stdout.buffer.write(expand_bytes(sys.argv[1]))
