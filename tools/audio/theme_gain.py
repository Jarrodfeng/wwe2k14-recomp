"""Wwise gain (dB) the game applies to each entrance theme before the mix: Sound volume + its parents' (lane 283).
  py tools/audio/theme_gain.py GAME_SOUND_DIR [OUT.csv]  -> music id, stream id, chain dB, chain
Numbers only. Not covered (unknown): bus volumes / RTPC curves (the game's music-volume option) and the master mix."""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import akpk, wwise_music_map as M, wwise_volume as V


def theme_gains(snd_dir):
    g, _ = M.build(os.path.join(snd_dir, 'Game_Sound_E_S.pck'))
    _, s = akpk.read(os.path.join(snd_dir, 'Game_Sound_M.pck'))
    snd = {e[0]: e for e in s}
    out = []
    for mid in range(1000):
        eid = M.fnv1_32_lower('Play_MUS_%04d_0_0' % mid)
        if eid not in g.objs or g.objs[eid][0] != 4:
            continue
        b = g.objs[eid][1]
        for k in range(struct.unpack('>I', b[:4])[0]):
            a = struct.unpack('>I', b[4 + 4 * k:8 + 4 * k])[0]
            if a not in g.objs or g.objs[a][0] != 3:
                continue
            tgt = struct.unpack('>I', g.objs[a][1][2:6])[0]
            # the sound objects under the target
            todo, seen = [tgt], set()
            while todo:
                o = todo.pop()
                if o in seen or o not in g.objs:
                    continue
                seen.add(o)
                t, body = g.objs[o]
                if t == 2:
                    src = struct.unpack('>I', body[8:12])[0]
                    if src in snd and snd[src][2] > 300000:
                        db, chain = V.chain_volume_db(g, o)
                        out.append((mid, src, round(db, 2), chain))
                elif t == 5:
                    todo += [c for c in g.ids_in(body) if c in g.objs and g.objs[c][0] in (2, 5)]
    return out


if __name__ == '__main__':
    rows = theme_gains(sys.argv[1])
    for r in rows:
        print(*r)
    if len(sys.argv) > 2:
        with open(sys.argv[2], 'w') as f:
            f.write('music_id,stream_id,chain_db\n')
            for r in rows:
                f.write('%d,%d,%s\n' % r[:3])
