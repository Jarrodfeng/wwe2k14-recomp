#!/usr/bin/env python3
"""wwise_music_chain.py - read the stock music effects chain from the PLAYER's own Wwise banks.

What it answers: which RoomVerb, Parametric EQs and bus gains a stock theme passes through
(docs\\systems\\audio.md section 7.1), as numbers. Reads game_data\\extracted\\sound\\Game_Sound_E_S.pck
(AKPK, big endian, bank format v65). Output is parameter VALUES only; no bank bytes are written anywhere.

Call:
  py tools\\audio\\wwise_music_chain.py [--pck PATH] [--json] [--mixer ID]
  py tools\\audio\\wwise_music_chain.py --selftest        (hand-made banks, no game files needed)

It mirrors src\\audio\\music_chain_params.h (the C++ reader the game uses); the two are checked against each
other by the same numbers. Exit code 0 = parsed, 2 = refused (not v65 / not found / damaged).
"""
import json
import os
import struct
import sys

MUSIC_MIXER_ID = 126675442      # actor-mixer every Play_MUS_* event sits under (lane 284, 856 of 856 events)
BANK_VERSION = 65               # BKHD version word the game's engine checks

T_SOUND, T_EVENT, T_ACTION, T_ACTORMIXER, T_BUS = 2, 4, 3, 7, 8
T_FXSHARE, T_FXCUSTOM = 18, 19

PLUG_ROOMVERB = 0x76
PLUG_PARAMEQ = 0x69


class BankError(Exception):
    pass


def u32(b, p):
    return struct.unpack_from('>I', b, p)[0]


def f32(b, p):
    return struct.unpack_from('>f', b, p)[0]


def u16(b, p):
    return struct.unpack_from('>H', b, p)[0]


# ---------------------------------------------------------------- AKPK / BKHD / HIRC
def read_pck_banks(path):
    """Yield (bank_id, folder, bytes) for every bank in an AKPK package."""
    with open(path, 'rb') as f:
        head = f.read(28)
        if len(head) < 28 or head[:4] != b'AKPK':
            raise BankError('not an AKPK package: ' + path)
        hs, ver, fl, bl, sl, el = struct.unpack('>IIIIII', head[4:28])
        f.seek(0)
        hdr = f.read(hs + 8)
        p = 28 + fl
        n = u32(hdr, p)
        if p + 4 + 20 * n > len(hdr):
            raise BankError('bank LUT overruns header')
        ents = [struct.unpack_from('>IIIII', hdr, p + 4 + 20 * i) for i in range(n)]
        for bid, blk, size, sb, folder in ents:
            f.seek(sb * blk)
            yield bid, folder, f.read(size)


def hirc_objects(bank):
    """Parse one bank. Returns [(type, id, body)]; raises BankError for a non-v65 bank."""
    if len(bank) < 12 or bank[:4] != b'BKHD':
        raise BankError('no BKHD')
    ver = u32(bank, 8)
    if ver != BANK_VERSION:
        raise BankError('bank version %d, only v%d is understood' % (ver, BANK_VERSION))
    p = 0
    out = []
    while p + 8 <= len(bank):
        tag = bank[p:p + 4]
        n = u32(bank, p + 4)
        if p + 8 + n > len(bank):
            raise BankError('chunk overruns bank')
        if tag == b'HIRC':
            q = p + 8
            cnt = u32(bank, q)
            q += 4
            for _ in range(cnt):
                t = bank[q]
                ln = u32(bank, q + 1)
                body = bank[q + 5:q + 5 + ln]
                if len(body) != ln or ln < 4:
                    raise BankError('HIRC object overruns chunk')
                out.append((t, u32(body, 0), body[4:]))
                q += 5 + ln
        p += 8 + n
    return out


# ---------------------------------------------------------------- object decoders (v65)
def node_fx(b, p):
    """FX list at p of a node body. Returns (fx[(slot, id, shareset)], p_after)."""
    p += 1                          # override-parent-fx
    n = b[p]
    p += 1
    fx = []
    if n:
        p += 1                      # bypass bits
        for _ in range(n):
            fx.append((b[p], u32(b, p + 1), b[p + 5]))
            p += 7
    return fx, p


def decode_mixer(b):
    fx, p = node_fx(b, 0)
    return dict(fx=fx, bus=u32(b, p), parent=u32(b, p + 4))


def decode_bus(b):
    parent = u32(b, 0)
    p = 4
    n = b[p]
    p += 1
    ids = list(b[p:p + n])
    p += n
    vals = [f32(b, p + 4 * i) for i in range(n)]
    p += 4 * n
    p += 10                          # kill/virtual/max-inst/override/channels/env
    p += 12                          # recovery, max duck, nducks
    nd = u32(b, p - 4)
    ducks = []
    for _ in range(nd):
        ducks.append(dict(target=u32(b, p), db=f32(b, p + 4), fade_down_ms=u32(b, p + 8),
                          fade_up_ms=u32(b, p + 12), curve=b[p + 16]))
        p += 17
    nfx = b[p]
    p += 1
    fx = []
    if nfx:
        p += 1
        for _ in range(nfx):
            fx.append((b[p], u32(b, p + 1), b[p + 5]))
            p += 7
    # rtpc list: u16 count, (rtpc id, param, curve id, scaling u8, u16 npts, pts of 12 bytes)
    nr = u16(b, p)
    p += 2
    rtpc = []
    for _ in range(nr):
        rid, pid, cid, sc, sz = u32(b, p), u32(b, p + 4), u32(b, p + 8), b[p + 12], u16(b, p + 13)
        p += 15
        pts = [(f32(b, p + 12 * j), f32(b, p + 12 * j + 4)) for j in range(sz)]
        p += 12 * sz
        rtpc.append(dict(rtpc=rid, param=pid, curve=cid, points=pts))
    return dict(parent=parent, volume_db=dict(zip(ids, vals)).get(0, 0.0), ducks=ducks, fx=fx, rtpc=rtpc)


def decode_fx(b):
    """FxShareSet / FxCustom body -> (plugin id, blob)."""
    plug = u32(b, 0)
    sz = u32(b, 4)
    return plug >> 16, b[8:8 + sz]


def decode_roomverb(blob):
    if len(blob) < 80 + 5 + 20 + 4 + 1 + 24 + 8:
        raise BankError('RoomVerb blob too short (%d)' % len(blob))
    names = ['decay_s', 'hf_damping', 'diffusion', 'stereo_width', 'f1_gain_db', 'f1_freq', 'f1_q',
             'f2_gain_db', 'f2_freq', 'f2_q', 'f3_gain_db', 'f3_freq', 'f3_q', 'front_db', 'rear_db',
             'center_db', 'lfe_db', 'dry_db', 'er_db', 'reverb_db']
    d = {n: f32(blob, 4 * k) for k, n in enumerate(names)}
    q = 80
    d['er_enable'] = blob[q]
    d['er_pattern'] = u32(blob, q + 1)
    o = q + 5
    for n in ['pre_delay_ms', 'room_size', 'er_feedback', 'density', 'room_shape']:
        d[n] = f32(blob, o)
        o += 4
    d['reverb_units'] = u32(blob, o)
    o += 4
    d['tone_enable'] = blob[o]
    o += 1
    for n in ['f1_pos', 'f1_curve', 'f2_pos', 'f2_curve', 'f3_pos', 'f3_curve']:
        d[n] = u32(blob, o)
        o += 4
    return d


def decode_parameq(blob):
    if len(blob) < 56:
        raise BankError('ParametricEQ blob too short (%d)' % len(blob))
    bands = []
    o = 0
    for _ in range(3):
        bands.append(dict(type=u32(blob, o), gain_db=f32(blob, o + 4), freq=f32(blob, o + 8),
                          q=f32(blob, o + 12), on=blob[o + 16]))
        o += 17
    return dict(bands=bands, out_db=f32(blob, o))


# ---------------------------------------------------------------- the chain
def index_objects(objs):
    idx = {}
    for t, i, body in objs:
        if t in (T_ACTORMIXER, T_BUS, T_FXSHARE, T_FXCUSTOM):
            idx.setdefault((t, i), body)
    return idx


def build_chain(idx, mixer_id=MUSIC_MIXER_ID):
    """idx: {(type,id): body}. Returns the chain dict (numbers only)."""
    if (T_ACTORMIXER, mixer_id) not in idx:
        raise BankError('music actor-mixer %d not found' % mixer_id)
    mx = decode_mixer(idx[(T_ACTORMIXER, mixer_id)])
    chain = dict(mixer_id=mixer_id, music_bus_id=mx['bus'], roomverb=None, roomverb_id=None, eq=[],
                 bus_db={}, ducks=[], volume_rtpc=None)
    # RoomVerb on the mixer (slot 0)
    for slot, fid, share in mx['fx']:
        key = (T_FXSHARE if share else T_FXCUSTOM, fid)
        if key not in idx:
            key = (T_FXCUSTOM if share else T_FXSHARE, fid)
        if key not in idx:
            continue
        plug, blob = decode_fx(idx[key])
        if plug == PLUG_ROOMVERB:
            chain['roomverb'] = decode_roomverb(blob)
            chain['roomverb_id'] = fid
    if chain['roomverb'] is None:
        raise BankError('no RoomVerb on the music actor-mixer')
    # walk up the bus tree
    bid = mx['bus']
    seen = set()
    levels = []
    while bid and bid not in seen and (T_BUS, bid) in idx:
        seen.add(bid)
        bus = decode_bus(idx[(T_BUS, bid)])
        levels.append((bid, bus))
        for slot, fid, share in bus['fx']:
            for key in ((T_FXSHARE, fid), (T_FXCUSTOM, fid)):
                if key in idx:
                    plug, blob = decode_fx(idx[key])
                    if plug == PLUG_PARAMEQ:
                        chain['eq'].append(dict(bus=bid, slot=slot, id=fid, **decode_parameq(blob)))
                    break
        bid = bus['parent']
    chain['bus_db'] = {('0x%08X' % b): bus['volume_db'] for b, bus in levels}
    chain['music_bus_db'] = levels[0][1]['volume_db'] if levels else 0.0
    chain['master_db'] = levels[-1][1]['volume_db'] if len(levels) > 1 else 0.0
    chain['eq'].sort(key=lambda e: e['slot'])
    for r in levels[0][1]['rtpc'] if levels else []:
        if r['param'] == 0:
            chain['volume_rtpc'] = dict(id=r['rtpc'], points=r['points'])
    # ducking: every bus whose duck list targets the music bus
    for (t, i), body in idx.items():
        if t != T_BUS:
            continue
        try:
            b = decode_bus(body)
        except (struct.error, IndexError):
            continue
        for d in b['ducks']:
            if d['target'] == mx['bus']:
                chain['ducks'].append(dict(ducker='0x%08X' % i, db=d['db'], fade_down_ms=d['fade_down_ms'],
                                           fade_up_ms=d['fade_up_ms'], curve=d['curve']))
    chain['ducks'].sort(key=lambda d: d['ducker'])
    return chain


def read_chain_from_pck(path, mixer_id=MUSIC_MIXER_ID):
    idx = {}
    nbank = 0
    for bid, folder, data in read_pck_banks(path):
        nbank += 1
        try:
            idx.update({k: v for k, v in index_objects(hirc_objects(data)).items() if k not in idx})
        except BankError as e:
            raise BankError('bank %d: %s' % (bid, e))
    chain = build_chain(idx, mixer_id)
    chain['banks_scanned'] = nbank
    return chain


FILTER_NAMES = {0: 'lowpass', 1: 'highpass', 2: 'bandpass', 3: 'notch', 4: 'low shelf', 5: 'high shelf', 6: 'peaking'}


def net_dry_db(c):
    rv = c['roomverb']
    return (c['music_bus_db'] + c['master_db'] + rv['dry_db'] + sum(e['out_db'] for e in c['eq']))


def print_chain(c):
    rv = c['roomverb']
    print('music actor-mixer %d -> bus 0x%08X (%.1f dB); master %.1f dB; banks scanned %s'
          % (c['mixer_id'], c['music_bus_id'], c['music_bus_db'], c['master_db'], c.get('banks_scanned', '?')))
    print('RoomVerb %s: decay %.2f s, HF damping %.2f, diffusion %.0f, width %.0f, ER %s, pre-delay %.0f ms, '
          'room size %.0f, density %.0f, room shape %.0f, units %d'
          % (c['roomverb_id'], rv['decay_s'], rv['hf_damping'], rv['diffusion'], rv['stereo_width'],
             'on' if rv['er_enable'] else 'off', rv['pre_delay_ms'], rv['room_size'], rv['density'],
             rv['room_shape'], rv['reverb_units']))
    for n, pos_key in ((1, 'f1_pos'), (2, 'f2_pos'), (3, 'f3_pos')):
        print('  tone filter %d: %+.1f dB @ %.0f Hz Q %.1f, position %d (%s)'
              % (n, rv['f%d_gain_db' % n], rv['f%d_freq' % n], rv['f%d_q' % n], rv[pos_key],
                 'off' if rv[pos_key] == 0 else 'on') + ('' if rv['tone_enable'] else ' [tone off]'))
    print('  levels: dry %.1f dB, ER %.1f dB, reverb %.1f dB' % (rv['dry_db'], rv['er_db'], rv['reverb_db']))
    for e in c['eq']:
        print('Parametric EQ %d on bus 0x%08X slot %d: out %+.1f dB' % (e['id'], e['bus'], e['slot'], e['out_db']))
        for k, bd in enumerate(e['bands']):
            print('  band %d: %-10s %+.1f dB @ %.0f Hz Q %.1f %s'
                  % (k + 1, FILTER_NAMES.get(bd['type'], str(bd['type'])), bd['gain_db'], bd['freq'], bd['q'],
                     'on' if bd['on'] else 'off'))
    if c['volume_rtpc']:
        pts = c['volume_rtpc']['points']
        print('music bus volume RTPC 0x%08X: %d points, %s' % (c['volume_rtpc']['id'], len(pts),
              ', '.join('%g->%.1f dB' % p for p in pts[:1] + pts[-1:])))
    for d in c['ducks']:
        print('duck by %s: %+.1f dB, %d ms down / %d ms up, curve %d'
              % (d['ducker'], d['db'], d['fade_down_ms'], d['fade_up_ms'], d['curve']))
    print('net dry gain (bus + master + EQ outs + RoomVerb dry): %.2f dB' % net_dry_db(c))


# ---------------------------------------------------------------- selftest: hand-made banks
def _be(*a):
    return struct.pack('>' + 'I' * len(a), *a)


def _obj(t, i, body):
    return bytes([t]) + _be(len(body) + 4, i) + body


def _bank(objs, ver=BANK_VERSION):
    hirc = _be(len(objs)) + b''.join(objs)
    bkhd = _be(ver, 1, 0)
    return b'BKHD' + _be(len(bkhd)) + bkhd + b'HIRC' + _be(len(hirc)) + hirc


def _fxslot(slot, fid, share):
    return bytes([slot]) + _be(fid) + bytes([share, 0])


def _bus_body(parent, vol, fx=(), ducks=(), rtpc=None):
    b = _be(parent) + bytes([1, 0]) + struct.pack('>f', vol)
    b += bytes(10) + _be(0) + struct.pack('>f', 0.0) + _be(len(ducks))
    for tg, db, fd, fu in ducks:
        b += _be(tg) + struct.pack('>f', db) + _be(fd, fu) + bytes([4])
    b += bytes([len(fx)])
    if fx:
        b += b'\x00' + b''.join(_fxslot(*x) for x in fx)
    if rtpc:
        b += struct.pack('>H', 1) + _be(0xEDF036D6, 0, 0x1234) + bytes([2]) + struct.pack('>H', len(rtpc))
        for x, y in rtpc:
            b += struct.pack('>ffI', x, y, 4)
    else:
        b += struct.pack('>H', 0)
    return b + _be(0)


def _roomverb_body(rv):
    blob = b''.join(struct.pack('>f', x) for x in (
        rv['decay_s'], rv['hf_damping'], rv['diffusion'], rv['stereo_width'],
        6.5, 300.0, 1.0, 0.0, 400.0, 1.0, -24.0, 7500.0, 8.5,
        0.0, -96.0, -96.3, 0.0, rv['dry_db'], -2.0, rv['reverb_db']))
    blob += bytes([0]) + _be(30) + b''.join(struct.pack('>f', x) for x in (
        rv['pre_delay_ms'], 100.0, 0.0, 28.0, 100.0)) + _be(8) + bytes([1])
    blob += _be(3, 0, 0, 1, 3, 1) + bytes(8) + bytes(44)
    return _be(0x760003, len(blob)) + blob + bytes(3)


def _eq_body(bands, out):
    blob = b''
    for t, g, fr, q, on in bands:
        blob += _be(t) + struct.pack('>fff', g, fr, q) + bytes([on])
    blob += struct.pack('>f', out) + bytes([0])
    return _be(0x690003, len(blob)) + blob + bytes(3)


def selftest():
    rv = dict(decay_s=2.3, hf_damping=6.45, diffusion=46.0, stereo_width=167.0, dry_db=-3.3, reverb_db=-17.0,
              pre_delay_ms=36.0)
    mixer = bytes([0, 1]) + b'\x00' + _fxslot(0, 75612494, 1) + _be(0x26160EEA, 0x2BB3807A) + bytes(30)
    objs = [
        _obj(T_ACTORMIXER, MUSIC_MIXER_ID, mixer),
        _obj(T_FXSHARE, 75612494, _roomverb_body(rv)),
        _obj(T_FXCUSTOM, 1060198217, _eq_body([(4, -4.0, 333.0, 1.0, 1), (4, -3.0, 5665.0, 63.3, 0),
                                               (5, -8.0, 1646.0, 1.0, 1)], 3.0)),
        _obj(T_FXCUSTOM, 616649844, _eq_body([(4, 0.0, 821.0, 1.0, 1), (6, -2.0, 1000.0, 1.0, 1),
                                              (5, -1.5, 763.0, 1.0, 1)], 1.0)),
        _obj(T_BUS, 0x26160EEA, _bus_body(0x2BB3807A, -9.0, rtpc=[(0.0, -96.3), (100.0, 0.0)])),
        _obj(T_BUS, 0x2BB3807A, _bus_body(0x26DF8722, 0.0, fx=[(1, 1060198217, 0), (2, 616649844, 0)])),
        _obj(T_BUS, 0x26DF8722, _bus_body(0, -2.0)),
        _obj(T_BUS, 0x213DB458, _bus_body(0x2BB3807A, -4.0, ducks=[(0x26160EEA, -5.0, 0, 1000)])),
    ]
    bank = _bank(objs)
    c = build_chain(index_objects(hirc_objects(bank)))
    assert abs(c['roomverb']['decay_s'] - 2.3) < 1e-5 and abs(c['roomverb']['pre_delay_ms'] - 36) < 1e-5, c['roomverb']
    assert abs(c['music_bus_db'] + 9) < 1e-6 and abs(c['master_db'] + 2) < 1e-6
    assert [e['out_db'] for e in c['eq']] == [3.0, 1.0]
    assert c['eq'][0]['bands'][2]['type'] == 5 and c['eq'][0]['bands'][1]['on'] == 0
    assert abs(net_dry_db(c) + 10.3) < 1e-4, net_dry_db(c)
    assert len(c['ducks']) == 1 and c['ducks'][0]['db'] == -5.0 and c['ducks'][0]['fade_up_ms'] == 1000
    assert c['volume_rtpc'] and c['volume_rtpc']['points'][1] == (100.0, 0.0)
    try:
        hirc_objects(_bank(objs, ver=62))
        raise SystemExit('selftest: non-v65 bank not refused')
    except BankError:
        pass
    try:
        hirc_objects(bank[:-5])
        raise SystemExit('selftest: truncated bank not refused')
    except BankError:
        pass
    print('selftest ok: hand-made 8-object bank parsed (RoomVerb, 2 EQ, 3 buses, 1 duck, RTPC); '
          'v62 and truncated banks refused')


def main(argv):
    if '--selftest' in argv:
        selftest()
        return 0
    pck = None
    mixer = MUSIC_MIXER_ID
    as_json = '--json' in argv
    if '--pck' in argv:
        pck = argv[argv.index('--pck') + 1]
    if '--mixer' in argv:
        mixer = int(argv[argv.index('--mixer') + 1])
    if pck is None:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.dirname(os.path.dirname(here))
        for cand in (os.path.join(root, 'game_data', 'extracted', 'sound', 'Game_Sound_E_S.pck'),
                     r'C:\Users\Jarrod\wwe2k14-recomp\game_data\extracted\sound\Game_Sound_E_S.pck'):
            if os.path.exists(cand):
                pck = cand
                break
    if not pck or not os.path.exists(pck):
        print('refused: Game_Sound_E_S.pck not found (pass --pck)', file=sys.stderr)
        return 2
    try:
        c = read_chain_from_pck(pck, mixer)
    except BankError as e:
        print('refused: %s' % e, file=sys.stderr)
        return 2
    if as_json:
        print(json.dumps(c, indent=1, default=str))
    else:
        print_chain(c)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
