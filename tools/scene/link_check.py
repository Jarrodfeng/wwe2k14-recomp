"""S103 TLF: cheap link cross-check for every tools\\scene\\build_*.ps1 that links an rf_*.cpp applier (no compile, under 1 s).

The failure it stops (tip reds s103_B / s103_C / s103_C3): an applier (rf_props.cpp, rf_wrestlers.cpp, ...) starts calling a public ModelCache method, the tool builds that
supply their own ModelCache stand-in instead of linking record_frame.cpp do not define it, and the tool only fails at LINK, in a gate lane far from the change.

For each build script: the .cpp files it names (resolved by file name under src\\ and tools\\scene\\; a line starting `$check` is compile-only and skipped), the ModelCache methods
record_frame.h declares out of line, the ones the linked files call (`models.X(` / `mc.X(` / `cache.X(`), and the ones defined (`ModelCache::X(` in a linked file or in a local
header a linked file includes). Calls minus definitions = exit 1, naming script, method and caller file.

  py -I tools\\scene\\link_check.py [--self-test]
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HDR = REPO / "src" / "renderer_scene" / "record_frame.h"
RECV = r"\b(?:models|models_|mc|cache|cache_)\s*(?:\.|->)\s*(\w+)\s*\("


def declared_methods(text):
    body = text[text.index("class ModelCache") :]
    body = body[: body.index("private:")]
    names = set()
    for ln in body.splitlines():
        code = ln.split("//")[0].strip()
        m = re.match(r"^[\w:<>\*&\s,]+?\b(\w+)\s*\([^)]*\)\s*(?:const)?\s*;$", code)
        if m and "{" not in code and not code.startswith(("return", "using")):
            names.add(m.group(1))
    names.discard("ModelCache")
    return names


def index_sources():
    idx = {}
    for root in (REPO / "src", REPO / "tools" / "scene"):
        for p in root.rglob("*.cpp"):
            idx.setdefault(p.name, []).append(p)
    return idx


def linked_files(script_text, idx):
    out = []
    for ln in script_text.splitlines():
        if ln.lstrip().startswith(("$check", "#")):
            continue
        for name in re.findall(r"['\"]?([\w\.]+\.cpp)['\"]?", ln):
            for p in idx.get(name, [])[:1]:
                if p not in out:
                    out.append(p)
    return out


def local_includes(p):
    res = []
    for m in re.finditer(r'^\s*#include\s+"([^"]+)"', p.read_text(encoding="utf-8", errors="replace"), re.M):
        q = (p.parent / m.group(1)).resolve()
        if q.exists() and q.suffix in (".h", ".inc", ".hpp"):
            res.append(q)
    return res


def check(script, declared, idx):
    text = script.read_text(encoding="utf-8", errors="replace")
    files = linked_files(text, idx)
    if not any(f.name.startswith("rf_") and f.parent.name == "renderer_scene" for f in files):
        return None
    defined, used = set(), {}
    for f in files:
        src = f.read_text(encoding="utf-8", errors="replace")
        for h in local_includes(f):
            src += "\n" + h.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"\bModelCache::(\w+)\s*\(", src):
            defined.add(m.group(1))
        if f.name.startswith("rf_") or f.parent.name == "tools" or f.parent.name == "scene":
            for m in re.finditer(RECV, f.read_text(encoding="utf-8", errors="replace")):
                if m.group(1) in declared:
                    used.setdefault(m.group(1), f.name)
    return sorted((n, who) for n, who in used.items() if n not in defined)


def main():
    declared = declared_methods(HDR.read_text(encoding="utf-8"))
    idx = index_sources()
    bad, n = [], 0
    for s in sorted((REPO / "tools" / "scene").glob("build_*.ps1")):
        r = check(s, declared, idx)
        if r is None:
            continue
        n += 1
        for name, who in r:
            bad.append(f"{s.name}: ModelCache::{name} is called ({who}) but defined by no linked file")
    if "--self-test" in sys.argv:   # planted fault: pretend the stub header is not linked -> must flag StoredNamed for c_driver
        stub = REPO / "tools" / "scene" / "modelcache_stub_common.h"
        # S103 GATEFIX: a check must never write a tracked file (the old plant rewrote the header, and normalised its line endings): the plant is applied
        # to what read_text returns for that one path, in memory.
        orig_read = Path.read_text

        def planted_read(self, *a, **k):
            t = orig_read(self, *a, **k)
            return t.replace("ModelCache::StoredNamed", "ModelCache::Renamed") if self.resolve() == stub.resolve() else t
        Path.read_text = planted_read
        try:
            r = check(REPO / "tools" / "scene" / "build_c_driver.ps1", declared, idx)
        finally:
            Path.read_text = orig_read
        if not r or "StoredNamed" not in [x[0] for x in r]:
            print("link_check self-test FAIL: the planted missing StoredNamed was not caught")
            return 2
        print("link_check self-test ok (planted missing StoredNamed caught)")
    for b in bad:
        print("FAIL", b)
    print(f"link_check: {n} build scripts link an rf_*.cpp applier, {len(declared)} ModelCache methods declared, {len(bad)} unresolved")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
