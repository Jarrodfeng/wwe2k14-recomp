"""NEEDS-MAIN (S103 WTG): a gate row that needs something only the MAIN checkout has is reported at once, never run, never a FAIL, never a 600 s wait.

Why: builder agents work in .claude\\worktrees\\agent-*, which hold only tracked files (no generated\\default, no configured out\\build, no SDK, no game_data).
A check that reads such a path from the tree it runs in fails there (FileNotFoundError, lld-link undefined symbols, empty logs) or waits out its timeout; the builder then
hands back "11 failures, probably infra". gate.py now asks this module before it runs a check:

  missing(c, root, main)  -> [what is missing under ROOT that MAIN has]   (empty when root is the main checkout, or nothing is missing)
  reclassify(root, main, out_lines, seconds) -> [..]   a check that FAILED at once on a root-relative path whose top folder MAIN has and this tree wholly lacks (strict: see the function)

What a check reads from its OWN tree is declared in the lane, per check:  "root_needs": ["game_data/extracted", "generated/default"]  (paths relative to the checkout).
A command that always reads such paths is also known here (IMPLICIT: tools\\build.ps1 needs generated\\default, the configured build folder and the SDK pins).
Paths a check reads from MAIN by design (out\\captures..., game_data\\ given as an argument: gate.py maps out/ and game_data/ arguments to MAIN) stay in "needs".
tools\\gate\\needs_lint.py flags a check whose script names generated\\default / game_data\\extracted without declaring either (the "undeclared need" rule).
The rows a builder hands back as `needs-main: lane.check, ...` are run by the main session: `py -I tools\\gate\\gate_main.py --rev <commit> --only <rows>` (no junctions: in gate-main a row that reads its own tree's generated\\ / game_data\\ is a plain FAIL: pass MAIN's path by argument or RDR_MAIN_CHECKOUT, the e2_check --game pattern)."""
import re
from pathlib import Path

# command text (regex over the joined argv) -> the paths the command reads from the tree it runs in
IMPLICIT = [
    (r'tools[/\\]build\.ps1', ['generated/default', 'out/build/win-amd64-release/CMakeCache.txt', 'tools/sdk_pins.json']),
    (r'tools[/\\]build_sdk\.ps1', ['thirdparty/rexglue-sdk-src']),
]

# a fast failure that names a missing file: the path is read from the output and compared with MAIN
_MISSING_RES = [
    re.compile(r"FileNotFoundError:[^\n]*?['\"]([^'\"\n]+)['\"]"),
    re.compile(r"No such file or directory:? ['\"]?([A-Za-z]:[^'\"\n]+)"),
    re.compile(r"cannot (?:find|open)[^\n]*?['\"]([^'\"\n]+)['\"]", re.I),
    re.compile(r"Cannot find path '([^'\n]+)'"),
]


def declared(c):
    out = list(c.get('root_needs', []))
    text = ' '.join(str(a) for a in c.get('cmd', []))
    for pat, paths in IMPLICIT:
        if re.search(pat, text):
            out += paths
    seen, res = set(), []
    for p in out:
        if p not in seen:
            seen.add(p)
            res.append(p)
    return res


def missing(c, root, main):
    """What c reads from ROOT that ROOT lacks but MAIN has. Nothing when root IS the main checkout (a path missing there is a plain SKIP / FAIL)."""
    root, main = Path(root), Path(main)
    if root.resolve() == main.resolve():
        return []
    return [n for n in declared(c) if not (root / n).exists() and (main / n).exists()]


_OWN_VERDICT = re.compile(r'(^|\s|\[)(PASS|FAIL|FAILED|ERROR)\b|AssertionError|\bassert\b', re.I)
MAX_RECLASSIFY_S = 20.0   # a check that dies on a missing file dies at once; one that ran longer did real work first


def reclassify(root, main, out_lines, seconds):
    """For a FAILED check: the folders its output says are missing, when ROOT lacks them wholly and MAIN has them (the failure is the worktree, not the code). [] otherwise.
    Strict, so a real failure never becomes NEEDS-MAIN (WTG review): only the LAST ~10 lines are read and the very last non-empty line must be the missing-file error;
    the top folder (generated/default, game_data/extracted, thirdparty/<dir>) must be wholly absent under ROOT (a half-present folder is a real failure); the check ran
    under MAX_RECLASSIFY_S; and it printed no PASS / FAIL / assertion line of its own anywhere (a fallback "No such file" followed by its verdict stays FAIL)."""
    root, main = Path(root).resolve(), Path(main).resolve()
    if root == main or seconds > MAX_RECLASSIFY_S:
        return []
    lines = [ln for ln in (out_lines or []) if ln.strip()]
    if not lines or any(_OWN_VERDICT.search(ln) for ln in lines):
        return []
    if not any(rx.search(lines[-1]) for rx in _MISSING_RES):
        return []
    hits = []
    for rx in _MISSING_RES:
        for m in rx.finditer('\n'.join(lines[-10:])):
            p = Path(m.group(1))
            try:
                rel = p.resolve().relative_to(root) if p.is_absolute() else p
            except ValueError:
                continue
            if len(rel.parts) < 2 or rel.parts[0] not in ('generated', 'game_data', 'thirdparty'):
                continue
            top = Path(*rel.parts[:2])
            if not (root / top).exists() and (main / top).exists():
                hits.append(rel.as_posix())
    return sorted(set(hits))
