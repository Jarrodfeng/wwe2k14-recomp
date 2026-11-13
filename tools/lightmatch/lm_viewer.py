r"""Lane 33: drive the Arena Viewer headless with a list of commands and read the replies (0 game runs; the viewer is not the game).

The viewer reads a script file (one JSON command per line) and answers one JSON line per command on stdout (selftest.py of lane 6 shows the
same pattern). `bank_dump` writes every mesh's draw constants as the viewer feeds them; `groups_from_bank` folds that into light groups
shaped like lm_extract.py's (class, effect, tuple) so the two sides compare directly.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]


def main_checkout():
    if os.environ.get('RDR_MAIN_CHECKOUT'):
        return Path(os.environ['RDR_MAIN_CHECKOUT'])
    if (REPO / 'game_data').exists():
        return REPO
    common = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True,
                            text=True).stdout.strip()
    return Path(common).parent if common else REPO


MAIN = main_checkout()


def viewer_exe(backend='vk'):
    """The viewer built from THIS worktree when it exists (a fix lands here), else the main checkout's."""
    env = os.environ.get('WWE_VIEWER_EXE')
    if env:
        return env
    name = 'wwe2k14_assets_rdr.exe' if backend == 'rdr' else 'wwe2k14_assets.exe'
    for base in (REPO, MAIN):
        p = base / 'out' / 'tools' / name
        if p.exists():
            return str(p)
    raise SystemExit(f'FAIL  no viewer exe ({name}) in {REPO} or {MAIN}: build it with tools\\assets\\build_viewer.ps1')


def run(cmds, backend='vk', timeout=300, workdir=None, env_extra=None, tag='lm'):
    """Runs the commands; returns (replies: list of dict, stderr). Raises SystemExit loudly when the viewer fails or answers fewer lines."""
    workdir = Path(workdir or (MAIN / 'out' / 's101' / 'lightmatch' / '_work'))
    workdir.mkdir(parents=True, exist_ok=True)
    script = workdir / f'{tag}.jsonl'
    script.write_text('\n'.join(json.dumps(c) for c in cmds) + '\n', encoding='utf-8')
    env = dict(os.environ, **(env_extra or {}))
    p = subprocess.run([viewer_exe(backend), '--backend', backend, '--game', str(MAIN / 'game_data' / 'extracted'), '--script', str(script),
                        '--exit', '--hidden'], capture_output=True, text=True, timeout=timeout, env=env)
    replies = []
    for line in p.stdout.splitlines():
        line = line.strip()
        if line.startswith('{'):
            try:
                replies.append(json.loads(line))
            except Exception:
                replies.append({'ok': False, 'raw': line[:200]})
    if p.returncode != 0:
        raise SystemExit(f'FAIL  viewer exit {p.returncode}: {p.stderr[-400:]}')
    return replies, p.stderr


def klass_of(effect, crowd=False, wrestler=False):
    if wrestler or effect.startswith('yCh_'):
        return 'wrestler'
    if crowd or effect.startswith('yAud'):
        return 'crowd'
    if effect.startswith('yBG_'):
        return 'arena'
    if effect.startswith('yProp_'):
        return 'prop'
    if effect.startswith('yMovie') or effect.startswith('yFlash'):
        return 'movie'
    return 'other'  # (yDefault: the viewer's crowd bodies carry the crowd flag; a game yDefault draw is not a crowd draw)


def groups_from_bank(bank):
    """-> list of {klass, effect, n, dir[4][3], col[4][3], sky[3], ground[3], limit[3], wt, hl_dir, hl_col, sets: sorted viewer set indices}"""
    gs = {}
    for d in bank['draws']:
        cb = d['cb']
        eff = d.get('shader', '?')
        k = klass_of(eff, d['mat'].get('crowd', False), d['mat'].get('wrestler', False))
        key = (k, eff, tuple(round(x, 4) for x in sum(cb['ldir'], [])), tuple(round(x, 4) for x in sum(cb['lcol'], [])),
               tuple(round(x, 4) for x in cb['sky'][:3]), tuple(round(x, 4) for x in cb['ground'][:3]))
        g = gs.get(key)
        if g is None:
            g = gs[key] = dict(klass=k, effect=eff, n=0, dir=[cb['ldir'][i][:3] for i in range(4)], col=[cb['lcol'][i][:3] for i in range(4)],
                               sky=cb['sky'][:3], ground=cb['ground'][:3], limit=cb['limit'][:3], hl_dir=cb['hl_dir'][:3], hl_col=cb['hl_col'][:3],
                               sets=set(), model=bank['models'][d['model']]['label'])
        g['n'] += 1
        g['sets'].add(d['light'])
    for g in gs.values():
        g['sets'] = sorted(g['sets'])
    return list(gs.values())
