"""Session 97: a matched-camera A/B against a game screenshot. Writes a viewer script that loads an arena, puts the camera
at the probed game camera (tools\\assets\\probe_camera.py), shoots 1280x720, runs the viewer, then saves game | ours side
by side. Local files only (screenshots and probe CSVs are never committed).

  py -I tools\\assets\\probe_ab.py CSV SWAP GAME_SHOT ARENA OUT_PNG [--extra JSONLINE ...]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))


def main():
    csv_path, swap, game_shot, arena, out_png = sys.argv[1:6]
    extra = [a for a in sys.argv[6:] if a != '--extra']
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'probe_camera.py'), csv_path, '--swap', swap],
                       capture_output=True, text=True)
    cam = [l for l in r.stdout.splitlines() if l.startswith('{')]
    if not cam:
        print('no camera command from probe_camera.py:', r.stdout, r.stderr)
        return 1
    ours = os.path.splitext(out_png)[0] + '_ours.png'
    script = os.path.splitext(out_png)[0] + '.jsonl'
    with open(script, 'w') as f:
        f.write('{"cmd":"load","kind":"arena","id":%d}\n' % int(arena))
        for e in extra:
            f.write(e + '\n')
        f.write(cam[-1] + '\n')
        f.write('{"cmd":"shot","path":"%s","w":1280,"h":720}\n' % ours.replace('\\', '/'))
    exe = os.path.join(REPO, 'out', 'tools', 'wwe2k14_assets.exe')
    p = subprocess.run([exe, '--game', os.environ.get('WWE_GAME', os.path.join(REPO, 'game_data', 'extracted')), '--script', script, '--exit',
                        '--hidden'], capture_output=True, text=True, timeout=120)
    bad = [l for l in p.stdout.splitlines() if '"ok":false' in l]
    if bad:
        print('viewer errors:', bad[:3])
    from PIL import Image
    g = Image.open(game_shot).convert('RGB').resize((1280, 720))
    o = Image.open(ours).convert('RGB').resize((1280, 720))
    w = Image.new('RGB', (2560, 720))
    w.paste(g, (0, 0))
    w.paste(o, (1280, 0))
    w.save(out_png)
    print('wrote', out_png)
    return 0


if __name__ == '__main__':
    sys.exit(main())
