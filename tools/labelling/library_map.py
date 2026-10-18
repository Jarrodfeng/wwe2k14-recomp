"""Library map (Session 10, after the bulk labelling was parked): which address ranges of the game image are library
code (Havok, Lua, Bink, audio, CRT, XDK ...) and which are the game's own, from evidence we already have. No agents.

  py tools/labelling/library_map.py [--page 0x1000] [--min-share 0.6]

Evidence per guest function (code index out/harness/index/index.db + db/*.json):
  rtti    the function is a slot of a vtable whose RTTI class name says the library (hk*/hcl* = Havok; C* / other
          game classes = game)
  string  a string the function references matches a library pattern (source file names, API / class names)
  label   its db name or summary (db/*.json incl. db/bulk_sonnet.json) names the library
Each function gets the libraries its evidence points to (one vote per kind). Pages of --page bytes get the library
that holds at least --min-share of the tagged functions on the page (and at least 2 of them); consecutive pages of the
same library merge into blocks, and runs of up to 2 untagged / mixed pages between two blocks of the same library are
absorbed (libraries are linked as contiguous object files).

Writes out/harness/labelling/library_map.json (blocks + per-function tags; local: derived from the game) and
docs/modules/library_map.md (the block table and the counts: our analysis; no game strings beyond library names).
Prints how many of the bulk selection's unlabelled functions fall into library blocks (what a later labelling phase
could skip or bulk-label).
"""
from __future__ import annotations

import argparse
import collections
import json
import re
import sqlite3
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
INDEX = REPO / "out" / "harness" / "index" / "index.db"
SELECTION = REPO / "out" / "harness" / "labelling" / "bulk" / "selection.json"
OUT_JSON = REPO / "out" / "harness" / "labelling" / "library_map.json"
OUT_MD = REPO / "docs" / "modules" / "library_map.md"

# Library patterns. Strings: matched against referenced string text; labels: against db name + summary.
STRING_RX = {
    "havok": r"\bhk[a-z]?[A-Z]\w+|\bhcl[A-Z]\w+|Havok|\\hk[A-Z]|hkBase|hkMemory",
    "lua": r"\blua_\w+|\bluaL_\w+|__index|__newindex|__gc\b|__mode\b|_LOADED|\bLUA_\w+|lua:|\.lua\b",
    "bink": r"\bbink\w*\.c\b|\bBink\w*|BINK|radmem|popmal",
    "audio": r"XAudio|X3DAudio|XACT|\bXMA\w*|Wwise|\bAK::|\bAk[A-Z]\w+|SoundEngine|\.bnk\b",
    "png_zlib": r"libpng|\bpng_\w+|\bPNG\b|inflate|deflate|zlib|\bIHDR\b|\bIDAT\b",
    "xdk": r"\bXam\w+|\bXContent\w*|\bXUI\w*|\bXui\w+|\bD3DDevice|\bXGraphics|XMemDecompress|XctdDecompression|\bXOnline",
    "crt": r"\\crt\\|\bmsvcrt|\bstd::|basic_string|\bbad_alloc\b|_invalid_parameter|\bprintf\b",
}
LABEL_RX = {
    "havok": r"\bhk[a-z_]|havok|\bhcl_",
    "lua": r"\blua\b|lua_|luaL",
    "bink": r"\bbink",
    "audio": r"xaudio|x3daudio|\bxma\b|wwise|audio",
    "png_zlib": r"\bpng|zlib|inflate|deflate",
    "xdk": r"\bxam_|xcontent|\bxui|xctd|xmem",
    "crt": r"\bcrt_|memcpy|memset|memmove|strlen|strcmp|strcpy|sprintf|malloc|atan2|\bpow\b",
}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--page", default="0x1000")
    ap.add_argument("--min-share", type=float, default=0.6)
    a = ap.parse_args()
    page = int(a.page, 0)
    con = sqlite3.connect(INDEX)
    funcs = sorted(r[0] for r in con.execute("SELECT addr FROM funcs WHERE has_body = 1"))
    votes: dict[int, collections.Counter] = collections.defaultdict(collections.Counter)
    kinds: dict[int, set] = collections.defaultdict(set)

    # rtti: vtable slot -> class name
    for func, cls in con.execute("""SELECT s.func, c.name FROM rtti_slot s JOIN rtti_vtable v ON v.vt = s.vt
                                    JOIN rtti_class c ON c.td = v.td"""):
        name = str(cls or "")
        lib = "havok" if re.match(r"^(hk|hcl)[A-Za-z]", name) else ("crt" if name.startswith("std::") else "game")
        votes[func][lib] += 1
        kinds[func].add("rtti")
    # strings
    srx = {k: re.compile(v) for k, v in STRING_RX.items()}
    for func, text in con.execute("SELECT r.func, s.text FROM strrefs r JOIN strings s ON s.addr = r.addr"):
        t = str(text or "")
        for lib, rx in srx.items():
            if rx.search(t):
                votes[func][lib] += 1
                kinds[func].add("string")
                break
    # labels
    lrx = {k: re.compile(v, re.I) for k, v in LABEL_RX.items()}
    for p in (REPO / "db").glob("*.json"):
        try:
            d = json.loads(p.read_text(encoding="utf-8"))
        except Exception:  # noqa: BLE001
            continue
        for e in d if isinstance(d, list) else d.get("entries", []):
            if not isinstance(e, dict) or not re.match(r"^0x8[0-9A-Fa-f]{7}$", str(e.get("address", ""))):
                continue
            text = f"{e.get('name', '')} {e.get('summary', '')}"
            for lib, rx in lrx.items():
                if rx.search(text):
                    addr = int(e["address"], 16)
                    votes[addr][lib] += 1
                    kinds[addr].add("label")
                    break

    tag = {f: votes[f].most_common(1)[0][0] for f in funcs if votes.get(f)}
    # pages
    pages: dict[int, collections.Counter] = collections.defaultdict(collections.Counter)
    page_funcs: dict[int, list] = collections.defaultdict(list)
    for f in funcs:
        pg = f // page
        page_funcs[pg].append(f)
        if f in tag:
            pages[pg][tag[f]] += 1
    lo, hi = funcs[0] // page, funcs[-1] // page
    page_lib = {}
    for pg in range(lo, hi + 1):
        c = pages.get(pg)
        if c:
            lib, n = c.most_common(1)[0]
            if n >= 2 and n / sum(c.values()) >= a.min_share:
                page_lib[pg] = lib
    # absorb short gaps between same-library pages
    keys = sorted(page_lib)
    for x, y in zip(keys, keys[1:]):
        if page_lib[x] == page_lib[y] and 1 < y - x <= 3:
            for pg in range(x + 1, y):
                page_lib.setdefault(pg, page_lib[x])
    # blocks
    blocks = []
    for pg in range(lo, hi + 1):
        lib = page_lib.get(pg)
        if lib is None:
            continue
        if blocks and blocks[-1]["lib"] == lib and blocks[-1]["end_page"] == pg - 1:
            blocks[-1]["end_page"] = pg
        else:
            blocks.append({"lib": lib, "start_page": pg, "end_page": pg})
    sel = json.loads(SELECTION.read_text(encoding="utf-8")) if SELECTION.exists() else {}
    sel_items = sel if isinstance(sel, list) else sel.get("functions") or sel.get("items") or []
    selected = {int(str(x.get("address")), 16) for x in sel_items if isinstance(x, dict) and x.get("address")}
    labelled = set()
    for p in (REPO / "db").glob("*.json"):
        try:
            d = json.loads(p.read_text(encoding="utf-8"))
        except Exception:  # noqa: BLE001
            continue
        for e in d if isinstance(d, list) else d.get("entries", []):
            if isinstance(e, dict) and re.match(r"^0x8[0-9A-Fa-f]{7}$", str(e.get("address", ""))):
                labelled.add(int(e["address"], 16))
    todo = selected - labelled
    lib_of_page = {}
    for b in blocks:
        for pg in range(b["start_page"], b["end_page"] + 1):
            lib_of_page[pg] = b["lib"]
    for b in blocks:
        fs = [f for pg in range(b["start_page"], b["end_page"] + 1) for f in page_funcs.get(pg, [])]
        b.update({"start": f"0x{b['start_page'] * page:08X}", "end": f"0x{(b['end_page'] + 1) * page:08X}",
                  "functions": len(fs), "tagged": sum(1 for f in fs if tag.get(f) == b["lib"]),
                  "labelled": sum(1 for f in fs if f in labelled), "unlabelled_selected": sum(1 for f in fs if f in todo)})
    by_lib = collections.Counter()
    todo_by_lib = collections.Counter()
    for f in funcs:
        lib = lib_of_page.get(f // page, "unassigned")
        by_lib[lib] += 1
        if f in todo:
            todo_by_lib[lib] += 1
    OUT_JSON.write_text(json.dumps({"page": page, "min_share": a.min_share, "blocks": blocks,
                                    "function_tags": {f"0x{f:08X}": tag[f] for f in sorted(tag)}}, indent=1),
                        encoding="utf-8")
    # markdown
    OUT_MD.parent.mkdir(parents=True, exist_ok=True)
    L = ["# Library map of the game image (Session 10, 2026-10-10)", "",
         "Generated by `py tools\\labelling\\library_map.py` from evidence already in the project: RTTI class names of",
         "vtable slots, the strings functions reference, and db labels (incl. the parked Sonnet bulk labels). Pages of",
         f"0x{page:X} bytes take the library holding >= {int(a.min_share * 100)}% of their tagged functions; same-library",
         "pages merge into blocks. **Heuristic: a lead for planning (what a labelling phase can skip or bulk-label), not",
         "proof that every function in a block belongs to that library.** 'game' = vtables of the game's own classes.", "",
         "## Functions per library (all functions with a body)", "",
         "| library | functions | of which in the parked bulk selection, still unlabelled |", "|---|---|---|"]
    for lib, n in by_lib.most_common():
        L.append(f"| {lib} | {n} | {todo_by_lib.get(lib, 0)} |")
    L += ["", "## Blocks (largest first)", "", "| start | end | library | functions | tagged | labelled | unlabelled (selection) |",
          "|---|---|---|---|---|---|---|"]
    for b in sorted(blocks, key=lambda b: -b["functions"])[:80]:
        L.append(f"| {b['start']} | {b['end']} | {b['lib']} | {b['functions']} | {b['tagged']} | {b['labelled']} | "
                 f"{b['unlabelled_selected']} |")
    if len(blocks) > 80:
        L.append(f"| ... | | {len(blocks) - 80} more blocks in out\\harness\\labelling\\library_map.json | | | | |")
    OUT_MD.write_text("\n".join(L) + "\n", encoding="utf-8")
    print(f"functions with a body: {len(funcs)}; tagged by evidence: {len(tag)}; blocks: {len(blocks)}")
    print("functions per library:", dict(by_lib.most_common()))
    print(f"unlabelled functions of the bulk selection: {len(todo)}; in library blocks:",
          {k: v for k, v in todo_by_lib.most_common() if k not in ("game", "unassigned")},
          "| game:", todo_by_lib.get("game", 0), "| unassigned:", todo_by_lib.get("unassigned", 0))


if __name__ == "__main__":
    main()
