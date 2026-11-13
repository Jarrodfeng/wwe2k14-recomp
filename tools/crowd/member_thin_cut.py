r"""Lane 272 (S101), piece M4 of docs\renderer\SKIP_GAME_WORK_MAP.md section 20 (Candidate M): emulate the crowd members' THIN record on a capture, offline. Writes a copy of a case zip in which, in every THIN frame, the
op-15 draw of every stands / floor member except the FIRST of each mesh run is REMOVED from the frame's op list, and a tally sidecar the way the hook would have written it (member_thin_core.h CountRing).
That is exactly what the hook does (member_thin_hook.h): the member's op-21 (c76-c78) is still recorded, so every register, the scene record's feed and the replay bookkeeping stay the game's; only the
indexed draw is not recorded. A capture's draw carries its full constant bank and state, so removing a draw edits nothing else (the check asserts the bank / state / texture sections of every cut frame are
byte-identical to the input). Frames are thin or full by the schedule the gate runs: the first --warmup frames with members are full, then every --k-th frame with members is a proof frame (full).

  py -I tools\crowd\member_thin_cut.py CASE.zip OUT.zip [--k 32] [--warmup 3] [--floor auto|on|off] [--plant NAME] [--decisions OUT.csv] [--counts OUT.csv]
RL1 (SKIP_GAME_WORK_MAP 16.9, 20.7): the sidecar walks the op list ONE OP 15 AT A TIME per call site (0x82518574 stands, 0x8276ED60 floor). A call is one op 15; the first call after the mesh changes (the
op 14's effect in a capture, which holds no op 14 as such) is kept, the others dropped; the world hash is the sum of FNV-1a over c76-c78 of every call's bank. `seen` (calls) is asserted != `runs` on
every frame that has a run of more than one member, and the tally is stamped hook_frame = stream guest_frame - 1 (member_thin_core.h kHookToStreamOffset).
Sidecar format (fview --crowd-members-thin-counts): hook_frame,site,seen,dropped,world per line, site 0 = stands, 1 = floor, BOTH sites on every frame that has a member draw (a site with no draw reads seen 0).
Plants (tests only):
  drop_first            the first member of each run is dropped too (the kept draw is the trigger, the anchor and the class material: a frame with no member draw has none of them)
  run_per_frame         only the first member of each SITE per frame is kept (one kept draw per site per frame: the other classes have no bank / picture)
  calls_collapsed       the tally counts a run as one call (RL1 `calls = runs`)
  offset                the tally is stamped with the stream frame number instead of frame - 1 (the record of the wrong frame; lane 36 +1 / lane 152 B1 / lane 175 B1)
  tiled_double          the tally is doubled (MSAA tiles: the crowd draw ran once per tile)
  floor_site_dropped_unobserved   the floor world sum is 0 (the floor op-15 dropped, its op-21 never noted)
  bank_nonuniform / picture_nonuniform   a FULL frame in which one non-first member draw of a run points at another draw's fragment bank / another class's texture rows (the uniformity census must see it)
  keep_all              CONTROL: nothing is cut (the output must equal the input replay)
  no_proof              no proof frame (every frame after the warm-up is thin)
Only the ops section (and, for the *_nonuniform plants, the draws table) is edited, in place (the table entry's count shrinks). Counts only; the output is a scratch file for under out\ (delete it after the comparison)."""
import argparse
import os
import struct
import sys
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cards_census as cc  # noqa: E402

SITES = {cc.SITE_BLOCK: 0, 0x8276ED60: 1}   # call site -> sidecar site
M64 = 0xFFFFFFFFFFFFFFFF
PLANTS = ['drop_first', 'run_per_frame', 'calls_collapsed', 'offset', 'tiled_double', 'floor_site_dropped_unobserved', 'bank_nonuniform', 'picture_nonuniform', 'keep_all', 'no_proof']


def fnv(words):
    h = 0xcbf29ce484222325
    for x in words:
        h = ((h ^ int(x)) * 0x100000001b3) & M64
    return h


def member_calls(f, floor_on):
    """-> list of (op position, site, mesh key, draw index) of every member op 15, in op order (one entry per CALL; never grouped)."""
    out = []
    if not len(f.draws) or not f.has_origins:
        return out
    site_col = f.origins['call_site']
    D = f.draws
    for k, (kind, idx) in enumerate(f.ops):
        if int(kind) != 2:
            continue
        idx = int(idx)
        site = SITES.get(int(site_col[idx]))
        if site is None or (site == 1 and not floor_on):
            continue
        d = D[idx]
        out.append((k, site, (int(d['vertex_offset']), int(d['vertex_bytes']), int(d['index_offset']), int(d['index_bytes'])), idx))
    return out


def split_runs(f, calls):
    """Run boundary = another op between two calls, another site, or another mesh (the effect of the game's op 14). -> list of lists of call indices."""
    runs, cur = [], []
    prev = None
    for n, (k, site, mesh, idx) in enumerate(calls):
        if prev is not None and (k != prev[0] + 1 or site != prev[1] or mesh != prev[2]):
            runs.append(cur)
            cur = []
        cur.append(n)
        prev = (k, site, mesh)
    if cur:
        runs.append(cur)
    return runs


def has_floor_records(cap):
    for num in cap.nums:
        raw = cap.record(num)
        if raw is not None and b'crowd:floor:' in raw:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('case')
    ap.add_argument('out')
    ap.add_argument('--k', type=int, default=32)
    ap.add_argument('--warmup', type=int, default=3)
    ap.add_argument('--floor', choices=['auto', 'on', 'off'], default='auto')
    ap.add_argument('--plant', choices=PLANTS)
    ap.add_argument('--decisions')
    ap.add_argument('--counts', help='sidecar for fview --crowd-members-thin-counts: hook_frame,site,seen,dropped,world')
    a = ap.parse_args()
    cap = cc.Cap(a.case)
    zin = cap.c.zf
    if zin is None:
        print('FAIL  the input must be a case zip')
        return 1
    floor_on = {'on': True, 'off': False, 'auto': None}[a.floor]
    if floor_on is None:
        floor_on = has_floor_records(cap)
    new_frames, dec, counts = {}, [], {}
    with_members = thin_frames = dropped_total = kept_total = calls_total = runs_total = frames_calls_eq_runs = 0
    for fi, name in enumerate(cap.frames):
        f = cap.frame(fi)
        calls = member_calls(f, floor_on)
        if not calls:
            continue
        n_m = with_members
        with_members += 1
        runs = split_runs(f, calls)
        seen = [0, 0]
        nruns = [0, 0]
        world = [0, 0]
        for k, site, mesh, idx in calls:
            seen[site] += 1
            world[site] = (world[site] + fnv(f.bank(int(f.draws[idx]['vs_bank']))[304:316].view('<u4'))) & M64
        for r in runs:
            nruns[calls[r[0]][1]] += 1
        for s in (0, 1):   # RL1: calls are not runs wherever a run holds more than one member
            if seen[s] and max((len(r) for r in runs if calls[r[0]][1] == s), default=0) > 1 and seen[s] == nruns[s]:
                print('FAIL  RL1: frame %d site %d: seen %d == runs %d (a collapsed count)' % (cap.nums[fi], s, seen[s], nruns[s]))
                return 1
            frames_calls_eq_runs += seen[s] > 0 and seen[s] == nruns[s]
        calls_total += sum(seen)
        runs_total += sum(nruns)
        proof = n_m < a.warmup or (a.plant != 'no_proof' and (n_m - a.warmup) % a.k == 0)
        stamp = int(f.guest_frame) - (0 if a.plant == 'offset' else 1)
        sc = list(seen)
        if a.plant == 'calls_collapsed':
            sc = list(nruns)
        wsum = list(world)
        if a.plant == 'floor_site_dropped_unobserved':
            wsum[1] = 0
        if proof or a.plant == 'keep_all':
            for s in (0, 1):
                counts[(stamp, s)] = [sc[s], 0, wsum[s]]
            if proof and a.plant in ('bank_nonuniform', 'picture_nonuniform') and len(runs) >= 2:   # the full frame keeps every draw; one non-first member points at another class's rows
                buf = bytearray(f.buf)
                es, cnt, off = f.secs[cc.pcap.SEC['draws']]
                D = np.frombuffer(buf, f.draws.dtype, cnt, off)
                done = False
                for r in runs:
                    if len(r) < 2 or done:
                        continue
                    victim = calls[r[1]][3]   # a NON-FIRST member of a run
                    if a.plant == 'bank_nonuniform':   # its fragment bank (no world in it) becomes the bank of any other draw of the frame that differs
                        mine = f.bank(int(D[victim]['ps_bank']))
                        for j in range(len(D)):
                            if j != victim and int(D[j]['ps_bank']) != int(D[victim]['ps_bank']) and not np.array_equal(f.bank(int(D[j]['ps_bank'])), mine):
                                D[victim]['ps_bank'] = D[j]['ps_bank']
                                done = True
                                break
                    else:   # picture_nonuniform: its texture rows become another class's
                        for r2 in runs:
                            if calls[r2[0]][1] != calls[r[0]][1] or r2 is r:
                                continue
                            donor = calls[r2[0]][3]
                            if D[victim]['tex_first'] != D[donor]['tex_first']:
                                D[victim]['tex_count'] = D[donor]['tex_count']
                                D[victim]['tex_first'] = D[donor]['tex_first']
                                done = True
                                break
                if done:
                    new_frames[name] = bytes(buf)
            dec.append((cap.nums[fi], 'full', sum(seen), 0))
            continue
        drop = np.zeros(len(f.ops), bool)
        site_first_seen = [False, False]
        for r in runs:
            for j, n in enumerate(r):
                site = calls[n][1]
                keep = (j == 0)
                if a.plant == 'run_per_frame':
                    keep = not site_first_seen[site]
                    site_first_seen[site] = True
                if a.plant == 'drop_first':
                    keep = False
                if not keep:
                    drop[calls[n][0]] = True
        ndrop = [0, 0]
        for k, site, mesh, idx in calls:
            ndrop[site] += int(drop[k])
        if not drop.any():
            for s in (0, 1):
                counts[(stamp, s)] = [sc[s], 0, wsum[s]]
            dec.append((cap.nums[fi], 'thin', sum(seen), 0))
            continue
        buf = bytearray(f.buf)
        es, cnt, off = f.secs[cc.pcap.SEC['ops']]
        keep_ops = np.frombuffer(bytes(f.buf), np.dtype([('kind', '<u4'), ('index', '<u4')]), cnt, off)[~drop]
        buf[off:off + 8 * len(keep_ops)] = keep_ops.tobytes()
        nsec = struct.unpack_from('<I', buf, 12)[0]
        for k in range(nsec):
            sid = struct.unpack_from('<I', buf, 32 + 24 * k)[0]
            if sid == cc.pcap.SEC['ops']:
                struct.pack_into('<Q', buf, 32 + 24 * k + 8, len(keep_ops))
        new_frames[name] = bytes(buf)
        for s in (0, 1):
            m = 2 if a.plant == 'tiled_double' else 1
            counts[(stamp, s)] = [sc[s] * m, ndrop[s] * m, (wsum[s] * m) & M64]
        thin_frames += 1
        dropped_total += int(drop.sum())
        kept_total += sum(seen) - int(drop.sum())
        dec.append((cap.nums[fi], 'thin', sum(seen), int(drop.sum())))
    with zipfile.ZipFile(a.out, 'w', zipfile.ZIP_STORED, allowZip64=True) as zout:
        for info in zin.infolist():
            zout.writestr(info.filename, new_frames.get(info.filename) or zin.read(info.filename))
    if a.counts:
        with open(a.counts, 'w') as fh:
            fh.write('# hook_frame,site (0 stands, 1 floor),op-15 calls seen,dropped,sum of FNV-1a over the 12 words of every member c76-c78\n')
            for (k, s) in sorted(counts):
                fh.write('%d,%d,%d,%d,%d\n' % (k, s, counts[(k, s)][0], counts[(k, s)][1], counts[(k, s)][2]))
    if a.decisions:
        with open(a.decisions, 'w') as fh:
            fh.write('# frame,kind,member draws before,dropped\n')
            for d in dec:
                fh.write('%d,%s,%d,%d\n' % d)
    print('thin: %d of %d frames with members cut (%d member draws dropped, %d kept as run anchors; %d calls in %d runs over %d frames, frames with calls == runs %d) floor=%s of %d frames%s -> %s' % (
        thin_frames, with_members, dropped_total, kept_total, calls_total, runs_total, with_members, frames_calls_eq_runs, 'on' if floor_on else 'off', len(cap.frames), (' (plant %s)' % a.plant) if a.plant else '', a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
