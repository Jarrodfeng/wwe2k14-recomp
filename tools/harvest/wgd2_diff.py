#!/usr/bin/env python3
r"""S105 C2: the whole-game diff, WIDER (extends tools\harvest\choice_diff.py, which holds stage / ring / screens). Families: struct (bgEtc.pac stage structures: cage, cell, chamber, Inferno,
ladder rigs), weapon (m2.pac weapon manager props), event (evtobj.pac entrance / event objects), char (every wrestler attire), crowd (every arena's audience). Offline, 0 game runs.

  py -I tools\harvest\wgd2_diff.py [--families struct,weapon,event,char,crowd] [--plant NAME] [--against NAME.json] [--out-dir DIR] [--name wgd2] [--limit-chars N]
  py -I tools\harvest\wgd2_diff.py --selftest        (no game files: the joiner on fixtures + a planted twin per family, and the sampler-name table against src\assets\jboy.h)

THE TWO SIDES.  Game side (this file, Python): the player's own pacs read with tools\wm30\pac.py: every asset of the family, per JBOY the mesh records (effect name +0x7C, word +0x8C, the sampler
parameters and the texture they name) and the texture directories of the same PACH / leaf. The rule it states [C: docs\research\s105\C3.md, sub_82C04DF0 0x82C0A880]: record word +0x8C bit 0x2
= one draw, back faces culled; clear = two-sided. Our side (tools\scene\wgd2_rig.cpp, C++): the SAME loaders and pure choice functions the live path calls (asset_load.h Load*, family_cull::Cull,
weapon_rows.h / weapon_plan.h StageTypeOfPach / IsLadderPach / IsRingStepsId / ...): nothing re-implemented. The rig prints one JSON line per asset; this file joins and classifies.
Per family the checks (a class is a FINDING, never a failure):
  row     the asset the files hold: not loaded by us / refused / has no type (struct: the game's directory rows 0-8 and 11-61 against StageTypeOfPach / IsLadderPach)
  meshes  the file's mesh records against the meshes we kept (dropped records, with the loader's own warning)
  cull    record bit 0x2 against family_cull::Cull (both switches on, their defaults); [reader] the independent word against the loader's word
  effect  the record's effect name against the material class we kept
  tex     per sampler the record names: the texture it names, against the texture we bound for the slot (names through SAMPLER_SLOT, lint-checked against jboy.h): bound a different one /
          unbound though a directory of the same PACH holds it / unbound and in no directory (the game finds it elsewhere or draws the fallback) / a sampler our loader does not read
  warn    our loader's own warnings for the asset (char, crowd: the game side has no record-level reader here: only the loader's word and its warnings are joined; marked [L])
  crowd   seat lines of the audience set (entry 21100) against the people we seated
Marks: [C] read in files / listing, [L] reasoning / shared reader. Rule 1: reads the player's files at run time; writes names, ids and counts only, under out\ (never committed)."""
import argparse
import collections
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'wm30'))

# sampler parameter name -> our slot key (src\assets\jboy.h ~723-733: the same names; lint-checked by --selftest). 'y0..y2' = the UVScroll layers (effect yBG_UVScroll*).
SAMPLER_SLOT = {'g_mDiffusePrm': 'd', 'sampDiffusePrm': 'd', 'g_mNormalFxPrm': 'n', 'sampNormalPrm': 'n', 'g_mMaskPrm': 'm', 'sampLightMapPrm': 'l', 'sampSpecularPrm': 's',
                'sampCubeRefPrm': 'c', 'sampRefMapPrm': 'f', 'sampDiffuse1Prm': 'y0', 'sampDiffuse2Prm': 'y1', 'sampDiffuse3Prm': 'y2',
                'texDiffuse': 'd', 'sampNoisePrm': 'z'}   # S105 C2L: yDefault's texDiffuse (jboy.h: effect yDefault only) and the yProp_PSM grain
NONARENA_SLOTS = ('g_mNoiseTilePrm', 'g_mSweatPrm', 'g_mNormalWklPrm')   # read only by non-arena (wrestler) models (jboy.h: `!o.arena`)
GAME_STRUCT_PACHS = list(range(0, 8)) + list(range(11, 62))   # S105 C2L: p8 (/STG/SNCE) is never asked for (sub_82576DD8 names 11 other paths; C2L.md verdict 4 [C]), so it is left out. docs\formats\bgetc_stg_structures.md "The directory" [C]: 0-7 the structures, 9 / 10 the weapon sets, 11-61 the per-arena ladder rigs
FAMILIES = ('struct', 'weapon', 'event', 'char', 'crowd')


# ---- the game's side: the player's files (pure readers) ------------------------------------------------------------------------------------------
def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def cstr(b):
    return ''.join(chr(c) if 32 <= c < 127 else '?' for c in bytes(b).split(b'\0', 1)[0])   # ascii only: the rig prints a '?' per non-ascii byte too


def jboy_records(blob):
    """-> [dict(effect, w8c, samplers {param: texture name})] of one JBOY leaf (layout: src\\assets\\jboy.h header; the record reader of tools\\arena\\arena_files.py parse_jboy)"""
    if blob[:4] != b'JBOY':
        return None
    body = blob[8:8 + be32(blob, 4)]
    nmesh, mtab, ntex, ttab = be32(body, 0x10), be32(body, 0x14), be32(body, 0x1C), be32(body, 0x24)
    out = []
    for mi in range(nmesh):
        rec = mtab + mi * 0xB8
        r = dict(effect=cstr(body[rec + 0x7C:rec + 0x8C]), w8c=be32(body, rec + 0x8C), samplers={})
        nparam, plist = be32(body, rec + 0x94), be32(body, rec + 0x98)
        for k in range(min(nparam, 512)):
            if plist + 4 * k + 4 > len(body):
                break   # the loader warns "material parameter list out of range"
            po = be32(body, plist + 4 * k)
            if po + 24 > len(body):
                continue   # "material parameter record out of range" (the loader skips it too)
            if be32(body, po + 16) >> 16 == 0x0F:
                ti = be32(body, po + 20)
                if ti < ntex:
                    r['samplers'][cstr(body[po:po + 16])] = cstr(body[ttab + 16 * ti:ttab + 16 * ti + 16])
        out.append(r)
    return out


def tex_dir_names(dec):
    """the texture names of a texture directory leaf (LE32 count, records of 32 bytes from +16: name[16], 'dds\\0'), or None when the leaf is not one"""
    if len(dec) < 0x30 or dec[:4] in (b'JBOY', b'PACH'):
        return None
    n = struct.unpack_from('<I', dec, 0)[0]
    if not 0 < n < 4096 or 16 + 32 * n > len(dec):
        return None
    names = set()
    for i in range(n):
        r = 16 + 32 * i
        if dec[r + 16:r + 20].lower() != b'dds\0':   # S105 C2L: the key-20 cube directories of m2 / bgEtc carry 'DDS\0' (upper case)
            return None
        names.add(cstr(dec[r:r + 16]))
    return names


def _asset(rows, key, recs, dirs, **kw):
    d = rows.setdefault(key, dict(records=None, dirs={}))
    if recs is not None:
        d['records'] = recs
    d['dirs'].update(dirs)
    d.update(kw)


def read_pac_family(fam, pac_mod):
    """the game's side of struct / weapon / event: {asset: dict(records, dirs {entry path: names})} from the pac's leaves"""
    rel = {'struct': 'pac/bg/bgEtc.pac', 'weapon': 'pac/m2.pac', 'event': 'pac/evt/evtobj.pac'}[fam]
    data = pac_mod.load(os.path.join(pac_mod.GAME, rel))
    rows, dirs_of = {}, collections.defaultdict(dict)
    leaves = []
    for name, dec in pac_mod.entries(data):
        parts = name.split('/')
        if fam == 'struct':
            if int(parts[0][1:]) > 61 or int(parts[0][1:]) in (9, 10):
                continue   # 62-64 the weapon sets, 65-67 effects: not structures (docs/formats/bgetc_stg_structures.md)
            if len(parts) == 3 and parts[1] == '1':   # pN/1/k = a model
                if int(parts[2]) <= 31:
                    leaves.append((parts[0], 'p%s/%s' % (parts[0][1:], parts[2]), jboy_records(dec)))
            elif len(parts) == 2:
                nm = tex_dir_names(dec)
                if nm:
                    dirs_of[parts[0]][parts[1]] = nm
        elif fam == 'weapon':
            if parts[0] in ('p0', 'p1') and len(parts) >= 4 and parts[1] == '1000':
                key = '/'.join(parts[2:-1])
                if len(parts) == 4 and parts[3] in ('0', '1', '2') and dec[:4] == b'JBOY':
                    leaves.append((name.rsplit('/', 1)[0], 's%s/%s/%s' % (parts[0][1:], parts[2], parts[3]), jboy_records(dec)))
                elif len(parts) == 5 and parts[2] == '14' and parts[4] == '0' and dec[:4] == b'JBOY':
                    leaves.append((name.rsplit('/', 1)[0], 's%s/14/sub%s' % (parts[0][1:], parts[3]), jboy_records(dec)))
                else:
                    nm = tex_dir_names(dec)
                    if nm:
                        dirs_of[name.rsplit('/', 1)[0]][parts[-1]] = nm
        else:   # event: pN/k (model k 0..9), pN/<id> (a directory)
            if len(parts) == 2 and parts[1].isdigit():
                if int(parts[1]) <= 9 and dec[:4] == b'JBOY':
                    leaves.append((parts[0], '%s/%s' % (parts[0][1:], parts[1]), jboy_records(dec)))
                else:
                    nm = tex_dir_names(dec)
                    if nm:
                        dirs_of[parts[0]][parts[1]] = nm
    for group, asset, recs in leaves:
        if recs is None:
            continue
        # a weapon's directories sit beside its model (group = the id's path); a structure's / event prop's in the PACH
        _asset(rows, asset, recs, {}, group=group)
    for asset, d in rows.items():
        d['dirs'] = dirs_of.get(d['group'], {})
    return rows


def crowd_seats(arena_id, pac_mod):
    """(person seat lines with a row defined, child-object lines) of an arena's audience text (entry 20000 / 21100; the tokeniser and row grammar of src\\assets\\asset_load.h AddCrowd read here again,
    kept in Python so the count is the files' own)"""
    p = os.path.join(pac_mod.GAME, 'pac', 'bg', 'bg%02d.pac' % arena_id)
    if not os.path.exists(p):
        return None
    text = None
    for name, dec in pac_mod.entries(pac_mod.load(p)):
        if name.endswith('/20000/21100'):
            text = dec
            break
    if text is None:
        return None
    ls, cur, cut = [], [], False
    for c in text:
        if c in (13, 10):
            if cur:
                ls.append(bytes(cur).decode('latin1'))
            cur, cut = [], False
        elif c == 0x3B:
            cut = True
        elif not cut and c not in (0x20, 0x09):
            cur.append(c)
    if cur:
        ls.append(bytes(cur).decode('latin1'))

    def ri(ch):
        return ord(ch) - 65 if 'A' <= ch <= 'Z' else 26 + ord(ch) - 97 if 'a' <= ch <= 'z' else -1
    rows = set()
    i = 0
    while i < len(ls) and ls[i][0] != '{':
        i += 1
    i += 1
    while i < len(ls) and ls[i][0] != '}':
        if ri(ls[i][0]) >= 0 and re.match(r'^\(-?[\d.]+(e[-+]?\d+)?,-?[\d.]+(e[-+]?\d+)?,-?[\d.]+(e[-+]?\d+)?,-?[\d.]+(e[-+]?\d+)?\)', ls[i][1:]):
            rows.add(ri(ls[i][0]))
        i += 1
    i += 1
    people = children = 0
    while i < len(ls):
        m = re.match(r'^\[(\d+)\](\d+)', ls[i])
        if not m or int(m.group(1)) > 19:
            i += 1
            continue
        while i < len(ls) and ls[i][0] != '{':
            i += 1
        i += 1
        while i < len(ls) and ls[i][0] != '}':
            s = ls[i]
            i += 1
            if len(s) >= 3 and ri(s[0]) in rows and s[1].isdigit() and s[2].isdigit():
                if len(s) > 4 and s[3] == '(' and s[4] in 'cw':
                    children += 1
                else:
                    people += 1
        i += 1
    return people, children


# ---- the joiner (pure: plain dicts in, classes out; --selftest enters here) -----------------------------------------------------------------------
def slot_key(pname, effect):
    k = SAMPLER_SLOT.get(pname)
    if pname == 'texDiffuse' and effect != 'yDefault':
        return None
    if k and k.startswith('y') and not (effect.startswith('yBG_UVScroll') or effect.startswith('yProp_UVScrol')):   # S105 C2L: + yProp_UVScroll
        return None
    return k


def ours_slot_name(mesh, key):
    if key.startswith('y'):
        return (mesh.get('y') or ['', '', ''])[int(key[1])]
    return mesh.get(key, '')


def sided(w8c):
    return 'one-sided' if w8c & 2 else 'two-sided'


def classify_asset(fam, game, ours):
    """-> [(field, label, detail)] one row per disagreement; game = the files' side of the asset (None = the family has no file-side reader: char / crowd), ours = the rig's JSON line."""
    out = []
    if not ours['ok']:
        why = re.sub(r'\d+', 'N', ours.get('err', ''))
        out.append(('row', '%s ours not loaded: %s' % (fam, why), ours.get('err', '')))
        return out
    meshes = ours['meshes']
    recs = game['records'] if game else None
    if recs is not None:
        kept = {m['r'] for m in meshes}
        dropped = [i for i in range(len(recs)) if i not in kept]
        if dropped:
            w = '; '.join(sorted({re.sub(r'\d+', 'N', x) for x in ours.get('warn', [])})[:2])
            out.append(('meshes', '%s file record not kept by our loader%s' % (fam, (' (%s)' % w) if w else ''), 'records %s of %d' % (dropped[:6], len(recs))))
        if len(recs) == 0:
            out.append(('meshes', '%s the file holds no mesh records' % fam, ''))
    for m in meshes:
        r = recs[m['r']] if recs is not None and 0 <= m['r'] < len(recs) else None
        if recs is not None and r is None:
            out.append(('meshes', '%s our mesh has no file record' % fam, 'record %s' % m['r']))
            continue
        w8c = r['w8c'] if r else m['cw']
        if r and r['w8c'] != m['cw']:
            out.append(('cull', '%s [reader] the loader\'s record word differs from the file\'s' % fam, '0x%X vs 0x%X' % (r['w8c'], m['cw'])))
        game_one = bool(w8c & 2)
        our_one = m['cu'] == 2
        if game_one != our_one:
            out.append(('cull', '%s cull game %s / ours %s' % (fam, sided(w8c), 'one-sided' if our_one else 'two-sided'), 'mesh %s' % m['r']))
        if r:
            if r['effect'] != m['sh']:
                out.append(('effect', '%s effect game %s / ours %s' % (fam, r['effect'], m['sh'] or '(none)'), 'mesh %s' % m['r']))
            alln = set()
            for d in (game['dirs'] or {}).values():
                alln |= {x.lower() for x in d}   # our texture lookup is case-insensitive (TexSet::Get)
            for pname, tn in r['samplers'].items():
                key = slot_key(pname, r['effect'])
                if key is None:
                    if pname in NONARENA_SLOTS:
                        out.append(('tex', '%s sampler %s not read for arena-kind models (loader rule: wrestler models only)' % (fam, pname), 'mesh %s' % m['r']))
                    else:
                        out.append(('tex', '%s sampler %s (effect %s) is not read by our loader' % (fam, pname, r['effect']), 'mesh %s texture %s' % (m['r'], tn)))
                    continue
                have = ours_slot_name(m, key)
                if have.lower() == tn.lower():
                    continue
                if have:
                    out.append(('tex', '%s slot %s: game %s / ours a different texture' % (fam, key, pname), '%s vs %s' % (tn, have)))
                elif tn.lower() == 'nameplate' and fam in ('event', 'struct', 'weapon'):
                    out.append(('tex', '%s slot %s runtime-made: the game builds the nameplate (a key-30 tga canvas + the holder name text, CWeaponEventSD::MakeWeapon sub_8272D710) [S105 C2L]' % (fam, key), 'mesh %s texture %s' % (m['r'], tn)))
                elif tn.lower() in alln:
                    out.append(('tex', '%s slot %s unbound though a directory of the same PACH holds it' % (fam, key), 'mesh %s texture %s' % (m['r'], tn)))
                else:
                    out.append(('tex', '%s slot %s unbound, texture in no directory of its PACH (fallback)' % (fam, key), 'mesh %s texture %s' % (m['r'], tn)))
        else:   # no record-level reader: the loader's own naming
            if m.get('dn') and not m.get('d'):
                out.append(('tex', '%s diffuse texture named but bound nowhere (the loader\'s name, no directory holds it) [L]' % fam, m['dn']))
    if fam in ('char', 'crowd'):
        for w in ours.get('warn', []):
            if '(info)' in w or w.startswith('crowd:') and 'seated' in w:
                continue
            if fam == 'crowd' and not w.startswith('crowd'):
                continue   # arena-level warnings (textures, lights) belong to the stage families, not the audience
            out.append(('warn', '%s our loader warns: %s' % (fam, re.sub(r'\d+', 'N', w)), w))
    return out


def classify_struct_dir(asset_rows, ours_rows):
    """the game's directory rows (0-8, 11-61) against the type / ladder functions of the rig's per-PACH row (a PACH with no model has a bare row)"""
    out = []
    bare = {o['asset']: o for o in ours_rows if o['fam'] == 'struct' and '/' not in o['asset']}
    for pn in GAME_STRUCT_PACHS:
        o = bare.get('p%d' % pn)
        if o is None:
            out.append(('p%d' % pn, ('row', 'struct PACH %d has no row in our run' % pn, '')))
            continue
        ours = o.get('ours') or {}
        if ours.get('type', -1) < 0 and not ours.get('ladder'):
            out.append(('p%d' % pn, ('row', 'struct PACH %d: our StageTypeOfPach / IsLadderPach give it no structure type' % pn, 'type %s ladder %s' % (ours.get('type'), ours.get('ladder')))))
    return out


def classify_weapon_ours(ours):
    """our choice functions for a weapon asset: the game draws every manager id; a row where our drawn flag is 0"""
    x = ours.get('ours') or {}
    if x.get('drawn') == 0 and ours['ok'] is False:
        return []   # already a 'row' class through classify_asset
    return []


def tabulate(rows_by_family):
    """rows_by_family = {fam: [(asset, field, label, detail)]} -> classes {(fam, label): dict(count, assets set, example)}"""
    classes = {}
    for fam, rows in rows_by_family.items():
        for asset, field, label, detail in rows:
            c = classes.setdefault(label, dict(fam=fam, field=field, count=0, assets=set(), example=None))
            c['count'] += 1
            c['assets'].add(asset)
            if c['example'] is None:
                c['example'] = (asset, detail)
    return classes


# ---- the run ------------------------------------------------------------------------------------------------------------------------------
RIG = os.path.join(ROOT, 'out', 'tools', 'wgd2_rig.exe')


def main_checkout():
    p = ROOT
    for _ in range(4):
        if os.path.isdir(os.path.join(p, 'game_data', 'extracted')):
            return p
        p = os.path.dirname(p)
    return ROOT


def run_rig(fam, plant, out_dir, extra=()):
    exe = RIG if os.path.exists(RIG) else os.path.join(main_checkout(), 'out', 'tools', 'wgd2_rig.exe')
    if not os.path.exists(exe):
        raise SystemExit('missing %s: run pwsh -NoProfile -File tools\\scene\\build_wgd2_rig.ps1' % exe)
    game = os.path.join(main_checkout(), 'game_data', 'extracted')
    f = os.path.join(out_dir, 'rig_%s%s.jsonl' % (fam, ('_' + plant) if plant else ''))
    cmd = [exe, game, fam] + (['--plant', plant] if plant else []) + list(extra)
    with open(f, 'w', encoding='utf-8') as fh:
        rc = subprocess.run(cmd, stdout=fh, stderr=subprocess.PIPE, text=True)
    if rc.returncode != 0:
        raise SystemExit('wgd2_rig %s failed (%d): %s' % (fam, rc.returncode, rc.stderr[:300]))
    return [json.loads(x) for x in open(f, encoding='utf-8') if x.strip()]


def family_rows(fam, ours_rows, pac_mod):
    """-> (rows [(asset, field, label, detail)], checked dict)"""
    rows = []
    checked = collections.Counter()
    game = read_pac_family(fam, pac_mod) if fam in ('struct', 'weapon', 'event') else None
    seen = set()
    for o in ours_rows:
        if fam == 'struct' and '/' not in o['asset']:
            continue
        g = game.get(o['asset']) if game is not None else None
        if game is not None and g is None:
            rows.append((o['asset'], 'row', '%s our rig loaded an asset the file side did not list' % fam, ''))
            continue
        seen.add(o['asset'])
        checked['assets'] += 1
        checked['meshes'] += len(o['meshes'])
        for field, label, detail in classify_asset(fam, g, o):
            rows.append((o['asset'], field, label, detail))
        if fam == 'char' and o.get('attires') == 0:
            rows.append((o['asset'], 'row', 'char the pac holds no model PACH (attire count 0)', ''))
    if game is not None:
        for a in sorted(a for a in set(game) - seen if game[a]['records'] is not None):
            rows.append((a, 'row', '%s the file holds the asset, our rig made no row for it' % fam, ''))
    if fam == 'struct':
        for a, (field, label, detail) in classify_struct_dir(game, ours_rows):
            rows.append((a, field, label, detail))
        checked['pach rows'] = len(GAME_STRUCT_PACHS)
    if fam == 'weapon':
        for o in ours_rows:
            x = o.get('ours') or {}
            if x.get('steps') and x.get('drawn') == 0:
                rows.append((o['asset'], 'row', 'weapon ours skips the ring steps (IsRingStepsId, the old drawer rule)', ''))
    if fam == 'crowd':
        for o in ours_rows:
            gs = crowd_seats(int(o['asset']), pac_mod)
            if gs is None:
                continue
            people, children = gs
            m = None
            for w in o.get('warn', []):
                mm = re.match(r'crowd: (\d+) people seated', w)
                if mm:
                    m = int(mm.group(1))
            checked['crowd seat lines'] += people
            if m is None and people:
                rows.append((o['asset'], 'crowd', 'crowd the file seats people, ours reports none', 'game %d' % people))
            elif m is not None and m != people:
                rows.append((o['asset'], 'crowd', 'crowd people seated: game %s / ours %s' % ('more' if people > m else 'fewer', 'fewer' if people > m else 'more'), 'game %d / ours %d' % (people, m)))
            if children:
                rows.append((o['asset'], 'crowd', 'crowd child-object seat lines (c / w) not drawn by us [U in the game]', '%d lines' % children))
    return rows, checked


def report(rows_by_family, checked_by_family, title):
    classes = tabulate(rows_by_family)
    order = sorted(classes, key=lambda c: (-classes[c]['count'], c))
    L = ['# wgd2: the game\'s files against our renderer\'s own choice code (%s)' % title,
         '# a class is a FINDING (files != ours by the stated rule); each is a lead for a probe, none is a verdict. [L] = rests on the loader\'s shared reader.', '']
    for fam in rows_by_family:
        n = sum(1 for c in classes.values() if c['fam'] == fam)
        tot = sum(c['count'] for c in classes.values() if c['fam'] == fam)
        L.append('%-7s assets %5d  meshes %6d  disagreement rows %5d in %2d classes' % (fam, checked_by_family[fam]['assets'], checked_by_family[fam]['meshes'], tot, n))
    L += ['', '## classes: rows | distinct assets | class | e.g. asset: detail']
    for c in order:
        x = classes[c]
        L.append('%6d | %5d | %s | e.g. %s: %s' % (x['count'], len(x['assets']), c, x['example'][0], x['example'][1]))
    return '\n'.join(L) + '\n', dict(totals={c: classes[c]['count'] for c in order}, assets={c: len(classes[c]['assets']) for c in order}, fam={c: classes[c]['fam'] for c in order},
                                     examples={c: list(classes[c]['example']) for c in order}, checked={f: dict(v) for f, v in checked_by_family.items()})


# ---- selftest ------------------------------------------------------------------------------------------------------------------------------
def _mesh(r=0, cw=6, cu=2, sh='yProp_Default', d='tex_a', **kw):
    m = dict(r=r, cw=cw, cu=cu, sh=sh, d=d, n='', l='', m='', s='', c='', f='', y=['', '', ''], dn=d)
    m.update(kw)
    return m


def _ours(fam, asset, meshes, ok=True, **kw):
    o = dict(fam=fam, asset=asset, ok=ok, err='' if ok else 'x', nmesh=len(meshes), ntex=1, warn=[], meshes=meshes)
    o.update(kw)
    return o


def _game(w8c=6, effect='yProp_Default', samplers=None, dirs=None):
    return dict(records=[dict(effect=effect, w8c=w8c, samplers=samplers if samplers is not None else {'sampDiffusePrm': 'tex_a'})], dirs=dirs if dirs is not None else {'0': {'tex_a'}})


def selftest():
    bad = 0

    def check(ok, what):
        nonlocal bad
        print('%s  %s' % ('PASS' if ok else 'FAIL', what))
        bad += 0 if ok else 1

    def labels(fam, g, o):
        return [x[1] for x in classify_asset(fam, g, o)]
    # clean pass: nothing disagrees
    for fam in FAMILIES[:3]:
        check(labels(fam, _game(), _ours(fam, 'a', [_mesh()])) == [], '%s clean: no class' % fam)
    check(labels('char', None, _ours('char', '1/0', [_mesh()])) == [], 'char clean: no class')
    check(labels('crowd', None, _ours('crowd', '4', [_mesh(cw=0, cu=0)])) == [], 'crowd clean: no class')
    # struct: C3's one-sided cull bit (the old rule: only stage slots followed the bit) -> a cull class; the ladder range of S103 LDR -> a row class
    check(any('cull game one-sided / ours two-sided' in x for x in labels('struct', _game(w8c=6), _ours('struct', 'p0/0', [_mesh(cu=0)]))), 'struct planted C3 cull_old: re-found as a cull class')
    d = classify_struct_dir({}, [dict(fam='struct', asset='p%d' % p, ours=dict(type=-1, ladder=0 if p == 61 else 1)) for p in GAME_STRUCT_PACHS])
    check(any('PACH 61' in x[1][1] for x in d) and not any('PACH 11:' in x[1][1] for x in d), 'struct planted ladder_short: PACH 61 re-found as a row class')
    # weapon: the PW-3 plant (textures read from key 11): every diffuse unbound though the directory holds it; the old drawer's ring-steps skip -> a row class
    check(any('unbound though a directory' in x for x in labels('weapon', _game(), _ours('weapon', 's0/5/0', [_mesh(d='')]))), 'weapon planted tex_first_11: re-found as an unbound-slot class')
    check(any('not loaded' in x for x in labels('weapon', _game(), _ours('weapon', 's0/29/0', [], ok=False))), 'weapon planted steps_skipped: re-found as a not-loaded row class')
    # event / char / crowd: the cull plant
    check(any('cull game' in x for x in labels('event', _game(w8c=2), _ours('event', '3/0', [_mesh(cw=2, cu=0)]))), 'event planted cull_old: re-found')
    check(any('cull game' in x for x in labels('char', None, _ours('char', '1/0', [_mesh(cw=6, cu=0)]))), 'char planted cull_old: re-found (the loader\'s word)')
    check(any('cull game' in x for x in labels('crowd', None, _ours('crowd', '4', [_mesh(cw=6, cu=0)]))), 'crowd planted cull_old: re-found (the loader\'s word)')
    # a reader fault (the loader's word differs from the file's) and a dropped record
    check(any('[reader]' in x for x in labels('struct', _game(w8c=6), _ours('struct', 'p0/0', [_mesh(cw=4, cu=0)]))), 'struct reader fault: the word differs from the file')
    check(any('not kept' in x for x in labels('struct', _game(), _ours('struct', 'p0/0', []))), 'struct dropped record found')
    # a sampler our loader never reads, and a texture in no directory
    check(any('not read by our loader' in x for x in labels('weapon', _game(samplers={'sampFoo': 't'}), _ours('weapon', 's0/5/0', [_mesh()]))), 'unread sampler found')
    check(any('no directory' in x for x in labels('weapon', _game(dirs={}), _ours('weapon', 's0/5/0', [_mesh(d='')]))), 'texture in no directory found')
    # crowd seats: the grammar on invented text (own bytes)
    class P:   # a fake pac module: one arena pac with the audience text
        GAME = os.path.join(ROOT, 'out', 'c2', 'fake_game')
    txt = b'{\nA(0,0,1,0);\n}\n[0]3\n{\nA00\nA01(c1)\nA02\n}\n'
    os.makedirs(os.path.join(P.GAME, 'pac', 'bg'), exist_ok=True)
    open(os.path.join(P.GAME, 'pac', 'bg', 'bg99.pac'), 'wb').write(b'x')
    P.load = staticmethod(lambda p: None)
    P.entries = staticmethod(lambda data: [('p0/20000/21100', txt)])
    check(crowd_seats(99, P) == (2, 1), 'crowd seat grammar: 2 people + 1 child object on invented text')
    # the sampler-name table against src\assets\jboy.h (lint: a renamed sampler cannot go unseen)
    jb = open(os.path.join(ROOT, 'src', 'assets', 'jboy.h'), encoding='utf-8', errors='replace').read()
    miss = [n for n in list(SAMPLER_SLOT) + list(NONARENA_SLOTS) if '"%s"' % n not in jb]
    check(not miss, 'sampler table is in jboy.h (missing: %s)' % ', '.join(miss))
    print('FAIL %d' % bad if bad else 'PASS selftest')
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--families', default=','.join(FAMILIES))
    ap.add_argument('--plant', default='', help='run the rig with a planted fault (cull_old, ladder_short, steps_skipped, tex_first_11)')
    ap.add_argument('--against', default='', help='a reference run\'s NAME.json: list the classes this run adds or grows (a plant\'s re-find)')
    ap.add_argument('--out-dir', default=os.path.join(ROOT, 'out', 'c2'))
    ap.add_argument('--name', default='wgd2')
    ap.add_argument('--limit-chars', type=int, default=0)
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--refind', action='store_true', help='after the clean run, run the planted twins (cull_old on struct / event; ladder_short on struct; steps_skipped + tex_first_11 on weapon): exit 1 unless each shows its class')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    import pac as pac_mod
    os.makedirs(a.out_dir, exist_ok=True)
    rows_by, checked_by = {}, {}
    for fam in a.families.split(','):
        extra = ['--limit', str(a.limit_chars)] if fam == 'char' and a.limit_chars else []
        ours = run_rig(fam, a.plant, a.out_dir, extra)
        rows_by[fam], checked_by[fam] = family_rows(fam, ours, pac_mod)
    text, matrix = report(rows_by, checked_by, 'plant %s' % (a.plant or 'none'))
    if a.against:
        ref = json.load(open(a.against))
        rt = ref.get('totals', {})
        text += '\n## against %s: classes new or grown (this run / the reference)\n' % a.against
        for c in sorted(matrix['totals'], key=lambda c: -(matrix['totals'][c] - rt.get(c, 0))):
            if matrix['totals'][c] > rt.get(c, 0):
                text += '%6d / %6d  %s\n' % (matrix['totals'][c], rt.get(c, 0), c)
    rc = 0
    if a.refind:
        twins = [('cull_old', 'struct,event', 'cull game'), ('ladder_short', 'struct', 'PACH 61'), ('steps_skipped', 'weapon', 'skips the ring steps'), ('tex_first_11', 'weapon', 'unbound though a directory')]
        text += '\n## planted twins (each must add its class to the clean run)\n'
        for plant, fams, needle in twins:
            rb, cb = {}, {}
            for fam in fams.split(','):
                rb[fam], cb[fam] = family_rows(fam, run_rig(fam, plant, a.out_dir), pac_mod)
            t2, m2 = report(rb, cb, 'plant ' + plant)
            added = {c: n for c, n in m2['totals'].items() if needle in c and n > matrix['totals'].get(c, 0)}
            text += '%s %-14s %s\n' % ('PASS' if added else 'FAIL', plant, '; '.join('%s +%d' % (c[:90], n - matrix['totals'].get(c, 0)) for c, n in added.items()) or 'class not re-found')
            rc = rc or (0 if added else 1)
    open(os.path.join(a.out_dir, a.name + '.txt'), 'w', encoding='utf-8').write(text)
    json.dump(matrix, open(os.path.join(a.out_dir, a.name + '.json'), 'w'), indent=1, sort_keys=True)
    print(text[:7000].encode('ascii', 'replace').decode('ascii'))
    return rc


if __name__ == '__main__':
    sys.exit(main())
