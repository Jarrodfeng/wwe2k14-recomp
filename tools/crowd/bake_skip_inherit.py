r"""Lane 155 review S2 (S101): the constant registers a draw after the bake passes INHERITS from the bake iterations, and the bank edits that emulate the cut without them. Offline, 0 game runs.

A capture bank is its base bank plus the 16-dword blocks (4 registers) written since (bank_defs / bank_patches). For a draw X after the bakes whose chain runs through a bake draw's bank, every block the chain's
writes did not touch holds what the LAST bake iteration left. With some iterations cut, the game would hold what the last UNCUT iteration (else the draw before the first bake pass, P) left. `inherit_edits` returns,
per such draw and stage, the blocks whose value changes under the cut and the values the cut leaves; `apply_edits` writes them into a copy of the frame (new full banks and bank defs appended at the end of the
frame file, the draws' bank indices repointed; nothing else moves)."""
import os
import struct

import numpy as np

import cards_census as cc

capread = cc.pcap.capread
NO_BASE = capread.NO_BASE


def chain_infos(f, bake_banks):
    """-> function info(j) = (through_bake, frozenset of blocks written on the way down to the bake bank), memoised along shared chain suffixes."""
    memo = {}

    def info(j0):
        stack, j = [], j0
        while True:
            if j in memo:
                res = memo[j]
                break
            if j in bake_banks:
                res = (True, frozenset())
                memo[j] = res
                break
            d = f.bdefs[j]
            if d['base'] == NO_BASE:
                res = (False, frozenset())
                memo[j] = res
                break
            stack.append(j)
            j = int(d['base'])
        for k in reversed(stack):
            d = f.bdefs[k]
            if res[0]:
                w = set(res[1])
                for q in range(int(d['patch_count'])):
                    w.add(int(f.bpatches[int(d['patch_first']) + q]['block']))
                res = (True, frozenset(w))
            memo[k] = res
        return memo[j0]
    return info


def root_bank(f, j):
    """The first bank of a chain (a base-less def): the register file as the frame began, for a frame whose bake passes come before any other draw."""
    while f.bdefs[j]['base'] != NO_BASE:
        j = int(f.bdefs[j]['base'])
    return f.bank(j)


def written_between(f, j_from, j_to):
    """Blocks patched on the base chain from bank j_from down to bank j_to (exclusive; j_to < 0 = down to the root). -> (set, linked)."""
    blocks, j, steps = set(), int(j_from), 0
    while j != j_to and steps < 100000:
        d = f.bdefs[j]
        for q in range(int(d['patch_count'])):
            blocks.add(int(f.bpatches[int(d['patch_first']) + q]['block']))
        if d['base'] == NO_BASE:
            return blocks, j_to < 0
        j, steps = int(d['base']), steps + 1
    return blocks, j == j_to


def cut_state(f, banks, mask, entry_words):
    """-> (the register file as it is after the LAST bake iteration really ran, the file the cut leaves) as (n, 4) u4 row arrays.
    The cut value of a block is the value after the last UNCUT iteration that wrote it, else the pass's entry value (review: per block, not 'the last uncut bake's bank')."""
    N = len(banks)
    real = f.bank(banks[-1]).view('u4').reshape(-1, 4)
    entry = entry_words.view('u4').reshape(-1, 4)
    cut = entry.copy().reshape(-1, 16)
    done = np.zeros(cut.shape[0], bool)
    for k in range(N - 1, -1, -1):
        if (mask >> k) & 1:
            continue
        prev = banks[k - 1] if k else -1
        w, linked = written_between(f, banks[k], prev)
        bank_k = f.bank(banks[k]).view('u4').reshape(-1, 16)
        if not linked:   # the chain does not link: every block that differs from the previous iteration (or the entry) counts as written
            ref = f.bank(prev).view('u4').reshape(-1, 16) if k else entry.reshape(-1, 16)
            w = w | set(int(i) for i in np.nonzero((bank_k != ref).any(axis=1))[0])
        for blk in w:
            if not done[blk]:
                cut[blk] = bank_k[blk]
                done[blk] = True
    return real, cut.reshape(-1, 4)


def inherit_edits(cap, f, mask, bake_passes, veff, poison=False):
    """-> list of (draw, key ('vs_bank'|'ps_bank'), {block: 4x4 u4 values the cut leaves}) for the draws after the last bake pass, for any number of bake passes."""
    D = f.draws
    if mask == 0:
        return []
    N = len(bake_passes)
    idx_ops, pos_ops = f.draw_ops()
    pos_of = {int(d): int(p) for d, p in zip(idx_ops, pos_ops)}
    first, last = bake_passes[0].pos, bake_passes[-1].pos
    bake_draws = [p.draws[0][1] for p in bake_passes]
    before = [int(d) for d in idx_ops if pos_of[int(d)] < first]
    after = [int(d) for d in idx_ops if pos_of[int(d)] > last and int(d) not in set(bake_draws)]
    if not after:
        return []
    P = before[-1] if before else None
    out = []
    for key in ('vs_bank', 'ps_bank'):
        banks = [int(D[x][key]) for x in bake_draws]
        entry = f.bank(int(D[P][key])) if P is not None else root_bank(f, banks[0])
        real, cut = cut_state(f, banks, mask, entry)
        changed = set(int(r) // 4 for r in np.nonzero((real != cut).any(axis=1))[0])
        if poison:   # test only: every block the bake wrote (against the entry state) is made 42.0 for the draws that inherit it: the emulation must then SEE a difference if any is consumed
            ent = entry.view('u4').reshape(-1, 4)
            changed = set(int(r) // 4 for r in np.nonzero((real != ent).any(axis=1))[0])
            if os.environ.get('POISON_ALL'):   # the positive control of the emulation itself: EVERY block of a later draw's bank that the chain did not write
                changed = set(range(real.shape[0] // 4))
        if not changed:
            continue
        info = chain_infos(f, set(banks))
        for d in after:
            through, patched = info(int(D[d][key]))
            if not through:
                continue
            blocks = changed - patched
            if blocks:
                out.append((d, key, {b: (np.full((4, 4), 0x42280000, '<u4') if poison else cut[4 * b:4 * b + 4].copy()) for b in blocks}))
    return out


def apply_edits(buf, f, edits):
    """-> a new frame file (bytes): for each edit a new full bank (the draw's current bank with the blocks replaced) and a base-less bank def, the draw repointed. Sections 13 (banks) and 17 (defs) are
    rewritten at the end of the file; every other offset is unchanged."""
    sec = cc.pcap.SEC
    buf = bytearray(buf)
    # A base-less def whose `full` index is past the table reads as an all-zero bank (pcap.FrameData.bank): appending banks would turn those into real (our) banks. Pad the table with zero banks up to the
    # highest index any def names, then append ours after the padding.
    top = max([int(x['full']) for x in f.bdefs if x['base'] == NO_BASE and int(x['full']) < 0x7FFFFFFF] + [len(f.full) - 1]) + 1
    pad = [np.zeros(1024, '<u4') for _ in range(top - len(f.full))]
    nfull, ndef = top, len(f.bdefs)
    full_new, defs_new = [], []
    es_off = {}
    nsec = struct.unpack_from('<I', buf, 12)[0]
    for k in range(nsec):
        sid, es, cnt, off = struct.unpack_from('<IIQQ', buf, 32 + 24 * k)
        es_off[sid] = (k, es, cnt, off)
    drow = es_off[sec['draws']]
    for d, key, blocks in edits:
        bank = f.bank(int(f.draws[d][key])).copy()
        for b, vals in blocks.items():
            bank[16 * b:16 * b + 16] = vals.reshape(-1)
        full_new.append(bank)
        defs_new.append((NO_BASE, nfull + len(full_new) - 1, 0, 0))
        ndef_idx = ndef + len(defs_new) - 1
        field = 40 if key == 'vs_bank' else 44
        struct.pack_into('<I', buf, drow[3] + 96 * d + field, ndef_idx)
    fb = np.ascontiguousarray(f.full['d']).astype('<u4').tobytes() + b''.join(b.tobytes() for b in pad) + b''.join(np.asarray(b, '<u4').tobytes() for b in full_new)
    db = b''.join(struct.pack('<IIII', int(x['base']), int(x['full']), int(x['patch_first']), int(x['patch_count'])) for x in f.bdefs) + b''.join(struct.pack('<IIII', *t) for t in defs_new)
    for sid, data, es in ((sec['constant_banks'], fb, 4096), (sec['bank_defs'], db, 16)):
        while len(buf) % 8:
            buf.append(0)
        off = len(buf)
        buf += data
        k = es_off[sid][0]
        struct.pack_into('<QQ', buf, 32 + 24 * k + 8, len(data) // es, off)
    return bytes(buf)
