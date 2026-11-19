"""Every [[midasm_hook]] in the manifest's config files must have been through the codegen (S103 BLF review 7).

A mid-asm hook declared in config\\*.toml (named by wwe2k14_manifest.toml `includes`) whose callback does not appear as
`extern void <name>(` under generated\\default leaves the feature silently OFF: the game builds and links without the hook
(the callback is only referenced by generated code). So the codegen must have run after the toml changed.

  py -I tools/gate/midasm_hooks_lint.py [--root .] [--generated DIR]   0 = every hook declared, 1 = a hook missing
  py -I tools/gate/midasm_hooks_lint.py --selftest                      the planted case: a hook the "generated" tree lacks must fail

In a worktree there is no generated\\ : the gate declares `needs generated/default` (NEEDS-MAIN, owed to the main session);
run by hand it falls back to the main checkout's generated\\default (RDR_MAIN_CHECKOUT, else the git common dir's parent).
"""
import os
import re
import subprocess
import sys
import tempfile

HOOK = re.compile(r'\[\[midasm_hook\]\](.*?)(?=\n\s*\[\[|\Z)', re.S)
NAME = re.compile(r'^\s*name\s*=\s*"([^"]+)"', re.M)
INCL = re.compile(r'"(config/[^"]+\.toml)"')


def hook_names(root):
    """{name: toml file} of every midasm hook in the manifest's included config files."""
    out = {}
    manifest = os.path.join(root, "wwe2k14_manifest.toml")
    files = []
    if os.path.exists(manifest):
        txt = open(manifest, encoding="utf-8").read()
        files = INCL.findall(txt)
    for rel in files:
        p = os.path.join(root, rel)
        if not os.path.exists(p):
            continue
        for body in HOOK.findall(open(p, encoding="utf-8").read()):
            m = NAME.search(body)
            if m:
                out[m.group(1)] = rel
    return out


def main_checkout(root):
    if os.environ.get("RDR_MAIN_CHECKOUT"):
        return os.environ["RDR_MAIN_CHECKOUT"]
    try:
        common = subprocess.run(["git", "-C", root, "rev-parse", "--path-format=absolute", "--git-common-dir"], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def declared(gen_dir, names):
    """The subset of `names` that appears as `extern void <name>(` in some file of gen_dir (stops reading once all are found)."""
    todo = {n: re.compile(rb"extern (?:void|bool|u?int\w*) " + re.escape(n.encode()) + rb"\(") for n in names}
    found = set()
    for fn in sorted(os.listdir(gen_dir)):
        if not fn.endswith((".cpp", ".h")) or len(found) == len(todo):
            continue
        with open(os.path.join(gen_dir, fn), "rb") as f:
            data = f.read()
        for n, rx in todo.items():
            if n not in found and rx.search(data):
                found.add(n)
    return found


def check(root, gen_dir):
    names = hook_names(root)
    if not os.path.isdir(gen_dir):
        print("midasm_hooks_lint: %s is missing (run the codegen)" % gen_dir)
        return 1
    ok = declared(gen_dir, list(names))
    bad = sorted(n for n in names if n not in ok)
    for n in bad:
        print("MISSING: [[midasm_hook]] %s (%s) has no `extern <type> %s(` under %s: run the rexglue codegen, the feature is silently off until then" % (n, names[n], n, gen_dir))
    print("midasm_hooks_lint: %d hook(s), %d declared, %d missing" % (len(names), len(ok), len(bad)))
    return 1 if bad else 0


def selftest():
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "config"))
        os.makedirs(os.path.join(d, "generated", "default"))
        open(os.path.join(d, "wwe2k14_manifest.toml"), "w").write('includes = ["config/a.toml"]\n')
        open(os.path.join(d, "config", "a.toml"), "w").write('[[midasm_hook]]\naddress = 0x1\nname = "hook_a"\nregisters = ["r3"]\n\n[[midasm_hook]]\naddress = 0x2\nname = "hook_b"\nregisters = ["r1"]\n')
        gen = os.path.join(d, "generated", "default")
        open(os.path.join(gen, "x.cpp"), "w").write("extern void hook_a(PPCRegister& r3);\n")
        planted = check(d, gen)   # hook_b was never generated: must fail
        open(os.path.join(gen, "y.cpp"), "w").write("extern void hook_b(PPCRegister& r1);\n")
        clean = check(d, gen)
    if planted == 1 and clean == 0:
        print("selftest PASS: a hook missing from the generated tree fails, the complete tree passes")
        return 0
    print("selftest FAIL: planted %s clean %s" % (planted, clean))
    return 1


def main():
    if "--selftest" in sys.argv:
        return selftest()
    root = sys.argv[sys.argv.index("--root") + 1] if "--root" in sys.argv else "."
    root = os.path.abspath(root)
    if "--generated" in sys.argv:
        gen = sys.argv[sys.argv.index("--generated") + 1]
    else:
        gen = os.path.join(root, "generated", "default")
        if not os.path.isdir(gen):
            gen = os.path.join(main_checkout(root), "generated", "default")
    return check(root, gen)


if __name__ == "__main__":
    sys.exit(main())
