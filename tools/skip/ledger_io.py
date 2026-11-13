"""S101 lane 92 (KL): readers for the ledger census: a capture frame (frame_NNNNNN.bin, adapter_frame.h + names.h sections), a scene record
(frame_NNNNNN.rsc, scene_record.h) and the fx registry (src\\renderer\\shaders\\spv\\fx_registry.h). Pure Python, no game memory; the capture
folder is game data and stays outside the repo (counts and register numbers are all that leave it)."""
import re
import struct
import pathlib

# ---- the frame -----------------------------------------------------------------------------------------------------------------------
SEC_OPS, SEC_TARGETS, SEC_CLEARS, SEC_DRAWS, SEC_COPIES, SEC_PRESENTS, SEC_FIXEDS, SEC_DYNAMICS, SEC_SAMPLERS, SEC_TEXSLOTS, SEC_LAYOUTS, \
    SEC_ELEMENTS, SEC_BANKS, SEC_BOOLLOOPS, SEC_CONTENTS, SEC_COPYEXTRAS, SEC_BANKDEFS, SEC_BANKPATCHES = range(1, 19)
SEC_ORIGINS, SEC_ROLES, SEC_PROV, SEC_STRINGS, SEC_WHERE, SEC_TEXTRUNS = 64, 65, 66, 67, 68, 69
CLASS_NAMES = ['unnamed', 'model_part', 'crowd', 'sprite', 'text', 'movie', 'particle', 'shadow', 'post', 'clear', 'other']


class Frame:
    """One capture frame: draws (with origin), expanded constant banks on demand, strings."""

    def __init__(self, path):
        self.path = pathlib.Path(path)
        b = self.b = self.path.read_bytes()
        magic, ver, self.cap_frame, sc, self.guest_frame, self.present_interval, self.flags = struct.unpack_from('<IIIIQII', b, 0)
        if magic != 0x52464352:
            raise ValueError('not a capture frame: %s' % path)
        self.S = {}
        for s in range(sc):
            sid, es, cnt, off = struct.unpack_from('<IIQQ', b, 32 + 24 * s)
            self.S[sid] = (es, cnt, off)
        self._banks = {}
        self.ndraws = self.S[SEC_DRAWS][1] if SEC_DRAWS in self.S else 0
        self.has_origins = SEC_ORIGINS in self.S

    def rec(self, sid, i):
        es, cnt, so = self.S[sid]
        return so + es * i

    def string(self, field):
        if not field or SEC_STRINGS not in self.S:
            return ''
        es, cnt, so = self.S[SEC_STRINGS]
        o = so + field - 1
        e = self.b.index(b'\0', o)
        return self.b[o:e].decode('utf-8', 'replace')

    def draw(self, i):
        o = self.rec(SEC_DRAWS, i)
        vs, ps, vsp, fixed, dyn, tf, tc, vb, pb = struct.unpack_from('<QQQIIIIII', self.b, o)
        return dict(vs=vs, ps=ps, fixed=fixed, dyn=dyn, tex_first=tf, tex_count=tc, vs_bank=vb, ps_bank=pb)

    def origin(self, i):
        if not self.has_origins:
            return None
        es = self.S[SEC_ORIGINS][0]
        o = self.rec(SEC_ORIGINS, i)
        klass, op, flags, call_site, pass_site, obj, part, tech, asset, widget = struct.unpack_from('<BBHIIIIIII', self.b, o)
        return dict(klass=klass, op=op, call_site=call_site, pass_site=pass_site, object=obj, part=part,
                    technique=self.string(tech), asset=self.string(asset))

    def bank(self, i):
        """The expanded 1024-dword constant bank of bank index i (tuple of 1024 uint32). Bank defs chain (a def patches an earlier one), so
        the chain is walked iteratively and every link is cached."""
        if i in self._banks:
            return self._banks[i]
        b = self.b
        if SEC_BANKDEFS not in self.S:
            es, _, so = self.S[SEC_BANKS]
            d = struct.unpack_from('<1024I', b, so + es * i)
            self._banks[i] = d
            return d
        chain = []
        j = i
        while j not in self._banks:
            base, full, pf, pc = struct.unpack_from('<IIII', b, self.rec(SEC_BANKDEFS, j))
            chain.append((j, base, full, pf, pc))
            if base == 0xFFFFFFFF:
                break
            j = base
        for (j, base, full, pf, pc) in reversed(chain):
            if base == 0xFFFFFFFF:
                es, _, so = self.S[SEC_BANKS]
                d = list(struct.unpack_from('<1024I', b, so + es * full))
            else:
                d = list(self._banks[base])
            for p in range(pf, pf + pc):
                o = self.rec(SEC_BANKPATCHES, p)
                blk = struct.unpack_from('<I', b, o)[0]
                d[16 * blk:16 * blk + 16] = struct.unpack_from('<16I', b, o + 4)
            self._banks[j] = tuple(d)
        return self._banks[i]

    def dynamic(self, i):
        o = self.rec(SEC_DYNAMICS, i)
        vp = struct.unpack_from('<6f', self.b, o)
        sc = struct.unpack_from('<iiII', self.b, o + 24)
        nd = struct.unpack_from('<12f', self.b, o + 136)
        return dict(viewport=vp, scissor=sc, ndc_scale=nd[0:3], ndc_offset=nd[3:6], guest_scale=nd[6:9], guest_offset=nd[9:12])


# ---- the scene record ----------------------------------------------------------------------------------------------------------------
SC_STRINGS, SC_CAMERA, SC_ASSETS, SC_OBJECTS, SC_BONES, SC_PARAMS, SC_LIGHTS, SC_SPRITES, SC_TEXTS, SC_MOVIES, SC_TARGETS, SC_ENGINE = range(1, 13)
SC_ARENA_WORLDS = 15  # S101 lane 130: ArenaWorld[] (68 bytes), pinned against scene_record.h by tools\scene\arena_world_test.cpp's static_assert
SC_LIGHT_STATES, SC_LIGHT_REFS = 13, 14  # S101 lane 109 (LS0): LightState[] (1232 bytes), LightRef[] (16 bytes)
SC_CASTER_NODES = 24  # S101 lane 217: uint32[] per-node palette hashes (FNV-1a 32 of the 64 matrix bytes) the CasterCall node_first indexes
SC_CASTER_MATS = 26  # S101 lane 226 (E5): Mat4[] (64 bytes) the plane pass entries' palette matrices (CasterCall flag 512 + node_first)
SC_SPHERE_OCC = 27  # S101 lane 242 (H3): SphereOccRow[] (200 bytes): 8 u32 (layout, flags, object, frame, count, component, character, calls), p[2][4], v[2][4], table[2][13] u32; pinned against scene_record.h by tools\sphere\sphere_rows_test.cpp
SC_CASTER_EXTRAS = 25  # S101 lane 221: CasterExtra[] (96 bytes, no pad) PARALLEL to the CasterCall[]: layout, flags, product[16], depth[2], pose_t[4]
SC_CASTER_CALLS, SC_CASTER_STATES = 22, 23  # S101 lane 202 (Caster D3 E0): CasterCall[] (64 bytes), LightState[] (1232 bytes) read at the pass entries; pinned against scene_record.h by tools\caster\caster_rows_test.cpp
SC_RING_ROWS, SC_RING_NODES = 20, 21 # S101 lane 194 (Ring D3 R0): RingRows[] (40 bytes), RingNodeRows[] (48 bytes), pinned against scene_record.h by tools\ring\ring_rows_test.cpp


class Record:
    """One scene record (.rsc): objects with world / palette ranges / light set, bones (vec4 rows), params, lights, camera."""

    def __init__(self, path):
        b = self.b = pathlib.Path(path).read_bytes()
        magic, ver, sc, src = struct.unpack_from('<IIIB', b, 0)
        if magic != 0x4E435352:
            raise ValueError('not a scene record: %s' % path)
        self.guest_frame = struct.unpack_from('<Q', b, 16)[0]
        self.S = {}
        for s in range(sc):
            sid, es, cnt, off = struct.unpack_from('<IIQQ', b, 40 + 24 * s)
            self.S[sid] = (es, cnt, off)

    def str(self, f):
        if not f or SC_STRINGS not in self.S:
            return ''
        o = self.S[SC_STRINGS][2] + f - 1
        return self.b[o:self.b.index(b'\0', o)].decode('utf-8', 'replace')

    def count(self, sid):
        return self.S[sid][1] if sid in self.S else 0

    def objects(self):
        """[dict(asset, klass, id, world(16 floats), bones_first, bones_count, params_first, params_count, light_set, tint, flags)]"""
        if SC_OBJECTS not in self.S:
            return []
        es, cnt, so = self.S[SC_OBJECTS]
        out = []
        for i in range(cnt):
            o = so + es * i
            asset, klass, layer, pad, flags, oid = struct.unpack_from('<IBBHII', self.b, o)
            world = struct.unpack_from('<16f', self.b, o + 16)
            bf, bc, pf, pc, mlo, mhi, ls = struct.unpack_from('<7I', self.b, o + 80)
            tint = struct.unpack_from('<4f', self.b, o + 108)
            out.append(dict(asset=asset, klass=klass, id=self.str(oid), flags=flags, world=world, bones_first=bf, bones_count=bc,
                            params_first=pf, params_count=pc, light_set=ls, tint=tint))
        return out

    def engine_objects(self):
        """[uint32 engine object address] parallel to objects() (section 12), or [] (lane 217: the caster calls name their object by this address)."""
        if SC_ENGINE not in self.S:
            return []
        es, cnt, so = self.S[SC_ENGINE]
        if es != 4:
            raise ValueError('EngineObject element_size %d, this reader knows 4' % es)
        return list(struct.unpack_from('<%dI' % cnt, self.b, so))

    def bones(self):
        if SC_BONES not in self.S:
            return []
        es, cnt, so = self.S[SC_BONES]
        return [struct.unpack_from('<16f', self.b, so + es * i) for i in range(cnt)]

    def lights(self):
        if SC_LIGHTS not in self.S:
            return []
        es, cnt, so = self.S[SC_LIGHTS]
        out = []
        for i in range(cnt):
            o = so + es * i
            st, kind, slot = struct.unpack_from('<IBB', self.b, o)
            d = struct.unpack_from('<3f', self.b, o + 8)
            c = struct.unpack_from('<3f', self.b, o + 20)
            rng = struct.unpack_from('<f', self.b, o + 32)[0]
            out.append(dict(set=st, kind=kind, slot=slot, dir=d, color=c, range=rng))
        return out

    def params(self):
        if SC_PARAMS not in self.S:
            return []
        es, cnt, so = self.S[SC_PARAMS]
        return [(self.str(struct.unpack_from('<I', self.b, so + es * i)[0]), struct.unpack_from('<4f', self.b, so + es * i + 4)) for i in range(cnt)]

    def light_states(self):
        """S101 lane 109: [dict(light=[224 floats], shadow_m=[3][16], shadow_f=[16], shadow_x=[2][8], map_wh=(w, h), slot, flags)], or [] (no section)."""
        if SC_LIGHT_STATES not in self.S:
            return []
        es, cnt, so = self.S[SC_LIGHT_STATES]
        if es != 1232:
            raise ValueError('LightState element_size %d, this reader knows 1232' % es)
        out = []
        for i in range(cnt):
            o = so + es * i
            light = list(struct.unpack_from('<224f', self.b, o))
            f = struct.unpack_from('<48f', self.b, o + 896)
            sf = list(struct.unpack_from('<16f', self.b, o + 896 + 192))
            sx = struct.unpack_from('<16f', self.b, o + 896 + 192 + 64)
            wh = struct.unpack_from('<2f', self.b, o + 1224 - 8)
            slot, flags = struct.unpack_from('<II', self.b, o + 1224)
            out.append(dict(light=light, shadow_m=[list(f[0:16]), list(f[16:32]), list(f[32:48])], shadow_f=sf, shadow_x=[list(sx[0:8]), list(sx[8:16])],
                            map_wh=wh, slot=slot, flags=flags))
        return out

    def light_refs(self):
        """S101 lane 109: [(object, ordinal, state, frame)], or [] (no section)."""
        if SC_LIGHT_REFS not in self.S:
            return []
        es, cnt, so = self.S[SC_LIGHT_REFS]
        if es != 16:
            raise ValueError('LightRef element_size %d, this reader knows 16' % es)
        return [struct.unpack_from('<IIII', self.b, so + es * i) for i in range(cnt)]

    def arena_worlds(self):
        """S101 lane 130: [dict(kind, layout, slot, object, frame, origin, clock, world(12 floats))] of section 15 (kScArenaWorlds, 68 bytes), or [] (no section).
        kind 1 = an object's rows (slot = the 1-based node index, origin = flags 1 foreign palette / 2 node assumed), kind 2 = the stage clock (origin = the stage set-up frame)."""
        if SC_ARENA_WORLDS not in self.S:
            return []
        es, cnt, so = self.S[SC_ARENA_WORLDS]
        if es != 68:
            raise ValueError('ArenaWorld element_size %d, this reader knows 68' % es)
        out = []
        for i in range(cnt):
            v = struct.unpack_from('<BBHIIIf12f', self.b, so + es * i)
            if v[1] != 1 or v[0] not in (1, 2):
                raise ValueError('ArenaWorld %d: layout %d kind %d, this reader knows layout 1, kinds 1 and 2' % (i, v[1], v[0]))
            out.append(dict(kind=v[0], layout=v[1], slot=v[2], object=v[3], frame=v[4], origin=v[5], clock=v[6], world=list(v[7:19])))
        return out

    def ring_rows(self):
        """S101 lane 194: [dict(kind, layout, klass, object, frame, call, keyed, r5, first_node, n_nodes, flags, alpha, rows=[n_nodes x 12 floats])] of sections 20 / 21 (kScRingRows, kScRingNodes),
        or [] (no section). One entry per ring model call; `call` = the LightRef ordinal; flags: 1 foreign r5, 2 count forwarded, 4 count unknown, 8 capped, 16 unreadable, 32 alpha unread."""
        if SC_RING_ROWS not in self.S:
            return []
        es, cnt, so = self.S[SC_RING_ROWS]
        if es != 40:
            raise ValueError('RingRows element_size %d, this reader knows 40' % es)
        pes, pcnt, pso = self.S.get(SC_RING_NODES, (48, 0, 0))
        if pes != 48:
            raise ValueError('RingNodeRows element_size %d, this reader knows 48' % pes)
        out = []
        for i in range(cnt):
            v = struct.unpack_from('<4B6IHHIf', self.b, so + es * i)
            if v[1] != 1 or v[0] != 1:
                raise ValueError('RingRows %d: layout %d kind %d, this reader knows layout 1 kind 1' % (i, v[1], v[0]))
            first, n = v[9], v[10]
            if first + n > pcnt:
                raise ValueError('RingRows %d: nodes %d+%d past the %d pool rows' % (i, first, n, pcnt))
            rows = [list(struct.unpack_from('<12f', self.b, pso + pes * (first + k))) for k in range(n)]
            out.append(dict(kind=v[0], layout=v[1], klass=v[2], object=v[4], frame=v[5], call=v[6], keyed=v[7], r5=v[8], first_node=first, n_nodes=n, flags=v[12], alpha=v[13], rows=rows))
        return out

    def caster_calls(self):
        """S101 lane 202 (Caster D3 E0): [dict(pass_, collapse, object, frame, ordinal, state, key, r5, palette, n_nodes, flags, map_a, pal_hash, camera)] of sections 22 / 23 (kScCasterCalls,
        64 bytes; kScCasterStates = LightState, 1232 bytes), or [] (no section). pass_: 1 map, 2 plane, 3 rim. flags: 1 r5 zero, 2 foreign r5, 4 no palette, 8 unreadable, 16 capped, 32 no state,
        64 collapse flag unread, 128 null block, 256 per-node hashes present (lane 217: node_hashes = list of n_nodes uint32, else None). state 0xFFFFFFFF = none (camera None). camera = dict(shadow_m, shadow_f, shadow_x) decoded as light_states() does."""
        if SC_CASTER_CALLS not in self.S:
            return []
        es, cnt, so = self.S[SC_CASTER_CALLS]
        if es != 64:
            raise ValueError('CasterCall element_size %d, this reader knows 64' % es)
        ses, scnt, sso = self.S.get(SC_CASTER_STATES, (1232, 0, 0))
        if ses != 1232:
            raise ValueError('CasterState element_size %d, this reader knows 1232' % ses)
        nes, ncnt, nso = self.S.get(SC_CASTER_NODES, (4, 0, 0))
        if nes != 4:
            raise ValueError('CasterNode element_size %d, this reader knows 4' % nes)
        xes, xcnt, xso = self.S.get(SC_CASTER_EXTRAS, (96, 0, 0))
        if xes != 96:
            raise ValueError('CasterExtra element_size %d, this reader knows 96' % xes)
        if xcnt not in (0, cnt):   # not parallel: C++ drops the caster tables (caster_errors); here the call list is empty, not an exception
            return []
        out = []
        for i in range(cnt):
            v = struct.unpack_from('<4B7IHHI3IIQ', self.b, so + es * i)
            if v[1] != 1 or v[0] != 1:
                raise ValueError('CasterCall %d: layout %d kind %d, this reader knows layout 1 kind 1' % (i, v[1], v[0]))
            if v[2] not in (1, 2, 3):
                raise ValueError('CasterCall %d: pass %d' % (i, v[2]))
            st = v[7]
            if st != 0xFFFFFFFF and st >= scnt:
                raise ValueError('CasterCall %d: state %d past the %d states' % (i, st, scnt))
            cam = None
            if st != 0xFFFFFFFF:
                o = sso + ses * st
                f = struct.unpack_from('<48f', self.b, o + 896)
                sf = list(struct.unpack_from('<16f', self.b, o + 896 + 192))
                sx = struct.unpack_from('<16f', self.b, o + 896 + 192 + 64)
                cam = dict(shadow_m=[list(f[0:16]), list(f[16:32]), list(f[32:48])], shadow_f=sf, shadow_x=[list(sx[0:8]), list(sx[8:16])])
            nh = None
            if v[13] & 256 and v[17] + v[11] <= ncnt:   # a node index past the pool drops only THIS call's node hashes (record_io.cpp drops the caster tables only; here the rest stays readable)
                nh = list(struct.unpack_from('<%dI' % v[11], self.b, nso + 4 * v[17]))
            mats = None
            mes, mcnt, mso = self.S.get(SC_CASTER_MATS, (64, 0, 0))
            if v[13] & 512 and mes == 64 and v[17] + v[11] <= mcnt:   # lane 226 (E5): the plane entry's palette matrices, 16 floats each in memory order (row-major as the game stores them)
                mats = [list(struct.unpack_from('<16f', self.b, mso + 64 * (v[17] + k))) for k in range(v[11])]
            ex = None
            if xcnt:   # lane 221: product = the thread slot's current camera (row-major as stored), depth = (G+300, G+304), pose_t = the map-A collapse point; flags 1 / 2 / 4 say which were read
                eo = xso + xes * i
                lay, fl = struct.unpack_from('<II', self.b, eo)
                if lay != 1:   # an unknown layout drops the caster tables (record_io.h CasterTablesProblem)
                    return []
                ex = dict(layout=lay, flags=fl, product=list(struct.unpack_from('<16f', self.b, eo + 8)), depth=list(struct.unpack_from('<2f', self.b, eo + 72)), pose_t=list(struct.unpack_from('<4f', self.b, eo + 80)))
            out.append(dict(pass_=v[2], collapse=v[3], object=v[4], frame=v[5], ordinal=v[6], state=st, key=v[8], r5=v[9], palette=v[10], n_nodes=v[11], flags=v[13], map_a=list(v[14:17]), pal_hash=v[18], camera=cam,
                            node_first=v[17], node_hashes=nh, extra=ex, mats=mats))
        return out

    def sphere_occ(self):
        """[dict(object, frame, count, component, character, calls, flags, ran=[bool,bool], p=[[x,y,z,r]]*2, v=[[x,y,z,strength]]*2, table=[[13 words]]*2)] (lane 242, H3):
        the 'CCSO' component's spheres as sub_826DF8E0 left them = the stream's VS c168 / c169 (p) and c170 / c171 (v). `frame` is the HOOK's guest frame (the record's header runs one ahead); [] = none recorded."""
        if SC_SPHERE_OCC not in self.S:
            return []
        es, cnt, so = self.S[SC_SPHERE_OCC]
        if es != 200:
            raise ValueError('SphereOcc element_size %d, this reader knows 200' % es)
        out = []
        for i in range(cnt):
            o = so + es * i
            lay, fl, ob, fr, n, comp, ch, calls = struct.unpack_from('<8I', self.b, o)
            if lay != 1 or n > 2:   # an unknown layout drops the table (record_io.h SphereTablesProblem)
                return []
            fl_ = struct.unpack_from('<16f', self.b, o + 32)
            tb = struct.unpack_from('<26I', self.b, o + 96)
            out.append(dict(object=ob, frame=fr, count=n, component=comp, character=ch, calls=calls, flags=fl, ran=[bool(fl & 8), bool(fl & 16)],
                            p=[list(fl_[0:4]), list(fl_[4:8])], v=[list(fl_[8:12]), list(fl_[12:16])], table=[list(tb[0:13]), list(tb[13:26])]))
        return out

    def camera(self):
        if SC_CAMERA not in self.S:
            return None
        o = self.S[SC_CAMERA][2]
        view = struct.unpack_from('<16f', self.b, o)
        proj = struct.unpack_from('<16f', self.b, o + 64)
        eye = struct.unpack_from('<3f', self.b, o + 128)
        return dict(view=view, proj=proj, eye=eye)


# ---- the fx registry -----------------------------------------------------------------------------------------------------------------
class Registry:
    """Variants (name, constant ranges {reg,count,offset,blend}, bools_offset, samplers) and the shader-hash -> variant map of fx_registry.h."""

    def __init__(self, path):
        t = pathlib.Path(path).read_text(encoding='utf-8')
        self.ranges = {}
        for m in re.finditer(r'kC_(\w+)\[\] = \{(.*?)\};', t):
            self.ranges[m.group(1)] = [tuple(int(x) for x in g) for g in re.findall(r'\{(\d+), (\d+), (\d+), (\d+)\}', m.group(2))]
        self.samplers = {}
        for m in re.finditer(r'kS_(\w+)\[\] = \{(.*?)\};', t):
            self.samplers[m.group(1)] = [(int(a), int(b)) for a, b in re.findall(r'\{(\d+), (\d+)\}', m.group(2))]
        vs = t[t.index('Variant kVariants[] = {'):] if 'Variant kVariants[] = {' in t else t[t.index('kVariants'):]
        vs = vs[:vs.index('};')]
        self.variants = []
        for m in re.finditer(r'\{"([\w]+)",', vs):
            self.variants.append(m.group(1))
        hs = t[t.index('HashEntry kHashes[] = {'):]
        self.hash2var = {}
        for m in re.finditer(r'\{0x([0-9A-Fa-f]+)ull, (\d+)\}', hs):
            self.hash2var[int(m.group(1), 16)] = int(m.group(2))

    def variant_of(self, h):
        i = self.hash2var.get(h)
        return None if i is None else self.variants[i]

    def read_regs(self, name):
        """Registers the variant reads: set of vec4 register numbers (the constant ranges' reg .. reg+count-1)."""
        regs = set()
        for reg, cnt, off, blend in self.ranges.get(name, []):
            regs.update(range(reg, reg + cnt))
        return regs
