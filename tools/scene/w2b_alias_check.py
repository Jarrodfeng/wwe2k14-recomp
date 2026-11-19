"""S103 W2B: src\\assets\\node_alias.h must equal what tools\\scene\\w2b_alias_table.py decodes from the player's own image (sub_82BD24C8's table fill). Needs the MAIN checkout (generated\\ and out\\harness\\index\\image.bin):
  py -I tools\\scene\\w2b_alias_check.py [--plant swap]     (the plant swaps two `to` names in the table text and must fail)
Exit 1 on any difference, an unresolved slot, or a tool failure."""
import pathlib
import re
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
tool = root / 'tools' / 'scene' / 'w2b_alias_table.py'
main = root.parents[2] if root.parent.name == 'worktrees' else root   # generated\ and out\ live in the main checkout
r = subprocess.run([sys.executable, '-I', str(tool)], capture_output=True, text=True, cwd=str(main))
if r.returncode != 0:
    print('FAIL: the table tool failed (unresolved slot or no image):', r.stdout[-400:], r.stderr[-400:])
    sys.exit(1)
tool_pairs = re.findall(r'\{"([^"]*)", "([^"]*)"\}', r.stdout)
head = (root / 'src' / 'assets' / 'node_alias.h').read_text(encoding='utf-8')
head_pairs = re.findall(r'^\s*\{"([^"]*)", "([^"]*)"\},', head, re.M)
if '--plant' in sys.argv and len(head_pairs) > 12:
    head_pairs[0], head_pairs[11] = (head_pairs[0][0], head_pairs[11][1]), (head_pairs[11][0], head_pairs[0][1])
if tool_pairs != head_pairs:
    diff = [(a, b) for a, b in zip(tool_pairs, head_pairs) if a != b]
    print(f'FAIL: node_alias.h differs from the decoded table ({len(tool_pairs)} vs {len(head_pairs)} pairs): {diff[:6]}')
    sys.exit(1)
print(f'ok: node_alias.h equals the decoded table ({len(tool_pairs)} pairs)')
