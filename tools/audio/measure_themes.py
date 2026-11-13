"""Loudness of the game's own entrance themes (lane 283): integrated LUFS (EBU R128 / BS.1770) and true peak per theme.
Finds the theme streams (wwise_music_map: Game_Sound_E_S.pck events -> Game_Sound_M.pck streams), converts each to a scratch
Ogg (wwise_ogg), measures it with ffmpeg's ebur128 filter and prints/writes ONLY numbers (music id, stream id, size, rate,
channels, LUFS, LRA, true peak dBTP). The scratch audio is deleted straight after each measurement; nothing is kept.
  py tools/audio/measure_themes.py GAME_SOUND_DIR CODEBOOKS.bin OUT.csv [SCRATCH_DIR] [--min-bytes N]
needs ffmpeg on PATH (a developer tool only; the shipped importer has its own meter, src/audio_import/loudness.h)."""
import os, re, struct, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import akpk, wwise_music_map as M, wwise_ogg as W


def measure(ogg_path):
    p = subprocess.run(['ffmpeg', '-hide_banner', '-nostats', '-i', ogg_path, '-af', 'ebur128=peak=true', '-f', 'null', '-'],
                       capture_output=True, text=True)
    t = p.stderr[p.stderr.rfind('Summary:'):]
    i = re.search(r'I:\s+(-?[\d.]+) LUFS', t)
    lra = re.search(r'LRA:\s+(-?[\d.]+) LU', t)
    pk = re.search(r'Peak:\s+(-?[\d.]+) dBFS', t)
    if not (i and pk):
        return None
    return float(i.group(1)), float(lra.group(1)) if lra else float('nan'), float(pk.group(1))


def main():
    snd_dir, lib_path, out_csv = sys.argv[1:4]
    scratch = sys.argv[4] if len(sys.argv) > 4 and not sys.argv[4].startswith('--') else tempfile.gettempdir()
    min_bytes = int(sys.argv[sys.argv.index('--min-bytes') + 1]) if '--min-bytes' in sys.argv else 300000
    lib = open(lib_path, 'rb').read()
    g, _ = M.build(os.path.join(snd_dir, 'Game_Sound_E_S.pck'))
    m_path = os.path.join(snd_dir, 'Game_Sound_M.pck')
    _, sounds = akpk.read(m_path)
    snd = {s[0]: s for s in sounds}
    themes = {}
    for mid in range(0, 1000):
        r = M.Graph.streams(g, 'Play_MUS_%04d_0_0' % mid, snd)
        for sid in r or []:
            if snd[sid][2] >= min_bytes:
                themes.setdefault(sid, []).append(mid)
    print(len(themes), 'distinct theme streams >= %d bytes' % min_bytes, flush=True)
    rows = []
    with open(m_path, 'rb') as f, open(out_csv, 'w') as out:
        out.write('stream_id,music_ids,bytes,rate,channels,seconds,lufs_i,lra,true_peak_dbfs\n')
        for sid, mids in sorted(themes.items()):
            _, off, size, _ = snd[sid]
            f.seek(off)
            raw = f.read(size)
            tmp = os.path.join(scratch, 'm283_%d.ogg' % sid)
            try:
                ogg, info = W.convert(raw, lib)
                with open(tmp, 'wb') as t:
                    t.write(ogg)
                r = measure(tmp)
            except Exception as ex:
                print(sid, 'FAILED', ex, flush=True)
                continue
            finally:
                if os.path.exists(tmp):
                    os.remove(tmp)
            if r is None:
                print(sid, 'no measurement', flush=True)
                continue
            row = (sid, ' '.join(map(str, mids)), size, info['rate'], info['channels'], round(info['samples'] / info['rate'], 1), r[0], r[1], r[2])
            rows.append(row)
            out.write(','.join(map(str, row)) + '\n')
            out.flush()
            print(*row, flush=True)
    return rows


if __name__ == '__main__':
    main()
