r"""q draworder: the DRAW ORDER of the colour pass, the game's against ours, per pair of draw families (S111 DRAWORDER). 0 game runs.

    py tools/q/q.py draworder [--flag DIFF|UNSETTLED|OK ...] [--fam NAME] [--ref REF] [--families] [--arena N [--cams N]] [--json]
    --arena N: beside each DIFF row "overlaps: yes (N rays) / no / unknown" from `q overlap` (tools/q/qoverlap.py): can the two families
    be on screen at one pixel, so the DIFF can change it.

GAME side (docs/systems/render_categories.md, read through qwhen's table layout): the colour walk sub_82C2FA10 visits the categories
ascending and, inside one, its list in append order (sub_82C30A10 appends, sub_82C2FFC8 list end; the re-key sub_82C30C10 appends to the
new list). Each family of FAMILIES names the text of its registration in the table's "What registers there" cell: its categories are the
rows that text is found in (the doc's number, never ours). Its position inside a category is known only where the doc states it (a
phrase FAMILIES quotes, e.g. 52: "flashes after the post"); anywhere else two families of one category are [U] -> UNSETTLED.
Its state (blend / no depth write / opaque) is the phrase FAMILIES quotes from the row's State cell; a phrase the doc no longer has
leaves the state unknown.

OURS side (the code at --ref, else the files on disk), read from the code, not written by hand:
  * FrontEnd::Record (front_end.cpp): the order of the stages and lists it records: run_stage(Stage::X), the opaque loop, the blended
    loop, the post pass, the flashes after the post (a planted twin's arm, `if (g_plant_...)`, is skipped);
  * the opaque list: the record order of rf::Classes (record_frame_classes.cpp, the kCRows names of rf_arena_live.cpp) and, when Record
    calls body_order::OrderOpaque, the turns of body_order.h (enum Turn order, TurnOf's predicates) ahead of it;
  * the blended list: SeeRank's ranks (see_through_order.h), then the record order (a rank SeeThroughBefore orders by place, 1 and, since
    S111 REKEYORDER2, 5, follows the game's walked lists 33 / 41 / 43 and the body place of 42: ours = the game's there);
  * the stage hooks: the Stage each hook draws at (its gate line) and, at one stage, AttachFrameHooks' push order (frame_hooks.cpp).
Which list / turn / rank / hook a family takes is FAMILIES' (its material class: character effect, a cycled slot, place_block ...);
where it then lands is the code's.

THE RULE (field `order`): for two families A, B:
  OK        when neither can let a pixel through: both draw opaque WITH depth write on both sides (the game's state and our list), so
            their order changes no pixel except which of two meshes at exactly equal depth wins: OK whatever the two orders are;
  DIFF      when the two sides draw them in opposite order AND either is blended or depth-write-off (on either side): only then can
            the order change a pixel (the later one is blended over / not hidden by the earlier);
  OK        when the order agrees;
  UNSETTLED when it could matter and the game's order is [U] (no doc row, the same category with no stated position, overlapping
            category ranges), our order is not read (an anchor lost, two families on one record row / one stage with no push order),
            or a family's state is unknown.
SCENES (--scene): `default` = no re-key. `entrance` = the entrance re-key sub_827FBFE0 (S111 DRAWORDER2): each family that has a `rekey` text
takes the categories the doc's "The re-key moves" row names (bodies 13 + slot -> 42, DRTR -> 43, CWDT / WATR -> 43 with CWDS staying 33,
an entrance prop's own node 26 -> 41 / 43), and ours draws a re-keyed body as draw_place::TakeLateBodies does (the blended list at the
character rank, when Record calls it and StampItems marks the moved item place_block). The own node's opaque meshes stay in our opaque list.
Limits: the host's frame rows after Record (54..69) are not in the set; the side walks (maps) are not colour-pass draws.
SeeRank is read line by line (qdraworder_extra.parse_see_rank): a rank line it cannot read is an ANCHOR problem, never skipped.
S111 REKEYORDER2: SeeRank's place term `k.place >= kPlaceAfterLateNodes` (the re-keyed rank 5) holds for a blended family whose categories
(in the scene asked) are all at or after walked list 1 (char_draw_list.h kDrawLists[1] = 41): in the entrance DRTR (43) and the re-keyed
body (42); in the default scene none.
Ranking of the DIFF rows (one line): sorted(rows, key=lambda r: (-r['sens'], -r['gap'], r['a'], r['b'])): both families see-through
first, then the wider category gap.
Selftest: py -I tools/q/qdraworder_selftest.py
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import qwhen   # noqa: E402  (parse_cats / parse_doc / layout_problems: the one reader of render_categories.md's layout)
import qdraworder_extra as X   # noqa: E402  (SeeRank's line reader, the re-key row, the gm / late-body facts)

DOC = 'docs/systems/render_categories.md'
RECORD = 'src/renderer_scene/front_end.cpp'
BODY_ORDER = 'src/renderer_scene/body_order.h'
CLASSES = 'src/renderer_scene/record_frame_classes.cpp'
CROWS = 'src/renderer_scene/rf_arena_live.cpp'
SEE = 'src/renderer_scene/see_through_order.h'
ATTACH = 'src/renderer_scene/frame_hooks.cpp'

PLACE_ROWS = 'src/renderer_scene/draw_place_rows.h'
DRAW_LISTS = 'src/scene_live/char_draw_list.h'   # S111 REKEYORDER2: kDrawLists (the walked lists: SeeRank's place term)
GM_STATE = 'src/renderer_scene/gm_node_state.h'
SCENES = ('default', 'entrance')
SENS = ('blend', 'nozw', 'screen')    # a state that lets the order change a pixel


class DrawOrderError(Exception):
    pass


class Fam:
    """one draw family. key = regex in the doc's "What registers there" cell (None = no doc row: [U]); see / see_re = its game state and the
    State-cell phrase that says so; pos / pos_re = its stated place inside its one category; ours = where our code draws it:
    ('opaque', (class rows), turn predicate | None), ('blended', (class rows), {rank predicates}), ('hook', hook id), ('phase', token);
    ours_see = our draw's state (default: opaque list 'opaque', blended list 'blend')."""

    def __init__(self, name, key, see=None, see_re=None, ours=None, ours_see=None, pos=None, pos_re=None, why='', see_cell=3, rekey=None, rekey_keep=False,
                 ent_ours=None):
        self.name, self.key, self.see, self.see_re, self.ours, self.pos, self.pos_re, self.why = name, key, see, see_re, ours, pos, pos_re, why
        # see_cell: the table cell the state phrase is in; rekey: regex in the entrance re-key row's "What moves" cell, its groups = the categories
        # the family moves to (rekey_keep: it keeps its old ones too); ent_ours: where our code draws it in the entrance scene (when late bodies are read)
        self.see_cell, self.rekey, self.rekey_keep, self.ent_ours = see_cell, rekey, rekey_keep, ent_ours
        self.ours_see = ours_see or {'opaque': 'opaque', 'blended': 'blend'}.get(ours[0] if ours else '', None)


# The families both sides name. The doc phrases are quoted from render_categories.md (each is checked against the doc every run).
FAMILIES = [
    Fam('pre3d_2d', r"Create a Finisher's sequence slot 5|Create a Finisher fade node", 'blend', r'sets its own blend', ('hook', 'scene2d_early'), 'blend',
        why='2D rows recorded below category 13 (walk_cat.h IsPre3D)'),
    Fam('cycled_slots', r'stage cycled slot groups', None, None, ('opaque', ('arena',), ('before_bodies',)),
        why='the arena slot-cycle groups (3, or 13 with the slot flag bit 64)'),
    Fam('chamber_c3', r'Elimination Chamber object', 'blend', r'chamber: sets none \(inherits the reset\)', ('opaque', ('props',), ('before_bodies',)),
        why='the chamber category-3 rows (rf_props marks kObjBeforeBodies)'),
    Fam('bodies', r"slot character's opaque node", 'blend', r'tested \+ blended', ('opaque', ('wrestlers',), ('char_slot', 'char_effect')),
        why='the wrestler opaque meshes (a character effect)', rekey=r'opaque node 13 \+ slot -> (\d+)',
        ent_ours=('blended', ('wrestlers',), frozenset({'place_block'}))),
    Fam('arena', r'stage group 0', 'blend', r'stage group: z 1, blend 1', ('opaque', ('arena',), None), why="the arena's opaque slots (group 0)"),
    Fam('ring_canvas', None, None, None, ('opaque', ('ring_early',), None), why='canvas + apron skirts: no doc row names their category'),
    Fam('screens', None, None, None, ('opaque', ('screens',), None), why='the stage screens: no doc row names their category'),
    Fam('ring', r'(?:^|; )ring \(`sub_825CD8D0`', 'opaque', r'blend off, alpha test off', ('opaque', ('ring_late',), None),
        why='posts, turnbuckles, covers, ropes'),
    Fam('props', r'CWeapon own node', 'blend', r'own node: z 1, blend 1', ('opaque', ('props',), None), why="the arena's own props (steps, desk)"),
    Fam('event_props', r'CWeapon own node', 'blend', r'own node: z 1, blend 1', ('opaque', ('models',), None), why='every other model object',
        rekey=r'own node 26 -> (\d+) \((\d+) for ids'),
    Fam('crowd', r'the crowd \(', 'opaque', r'crowd: saves cull.*blend off', ('hook', 'crowd'), 'opaque', why='LiveCrowd (members, seats, cards)'),
    Fam('stage_s', r'stage group `s`', 'blend', r'as 22', ('blended', ('arena',), frozenset()), why='the arena `s` (translucent) slots'),
    Fam('drtr', r"each character's DRTR", 'blend', r'DRTR: blend on', ('blended', ('wrestlers',), frozenset({'character'})),
        why="the wrestlers' translucent parts", rekey=r'its DRTR 33 -> (\d+)'),
    Fam('cwdt', r"CWeapon's CWDT", 'nozw', r'CWDT / CWDS: z write 0', ('blended', ('props', 'models'), frozenset({'place_block'})),
        why="a CWeapon node's category-33 pass", rekey=r'its CWDT and WATR 33 -> (\d+)', rekey_keep=True),
    Fam('stage_a', r'stage group `a`', 'blend', r'stage group as 22', ('blended', ('arena',), frozenset({'blend2'})),
        why='the arena `a` (additive) slots'),
    Fam('chamber_fences', r'chamber CDTB', 'nozw', r'chamber: blend on code 0, alpha test GEQUAL 1, z write off',
        ('blended', ('props',), frozenset({'late_node'})), why='CDTB chain fences (kObjNodeLate)'),
    Fam('chamber_glass', r'chamber CDTA', 'nozw', r'chamber as 35', ('blended', ('props',), frozenset({'late_node'})),
        why='CDTA pod glass (kObjNodeLate)', pos=0, pos_re=r'pod glass, spotlight code 1'),
    # S111 DRAWORDER2: the gm draw manager's category-36 rows (gm_node_state.h GmNodeObjectFlags: the GMGA pod glass chem_glass02_ma, a faded gm object)
    Fam('gm_cat36', r'the gm draw manager \(`sub_827C7BD8` \+1636: node \+4 is 36', 'nozw', r'gm manager at 36: .*z write 0', ('blended', ('props',), 'gm_preds'),
        why='gm set objects drawn in category 36 (key 1500+ or MdlAlpha < 1)'),
    # S111 DRAWORDER2: the chamber spotlight k 8 (blend code 1 = SRC_ALPHA / ONE), the last draw of CDTA: SeeRank's LateAdditive rank
    Fam('late_additive', r'chamber CDTA', 'blend', r'spotlight code 1', ('blended', ('props',), frozenset({'blend2', 'late_node'})),
        why='CDTA spotlight k 8 (kObjNodeLate, additive), last in CDTA', see_cell=1, pos=1, pos_re=r'pod glass, spotlight code 1'),
    Fam('scene2d', r'the entrance flash', 'nozw', r'entrance flash T_Rect z off', ('hook', 'scene2d_late'), 'blend',
        why='in-scene 2D rows: gm flashes, lens flare'),
    Fam('particles', r'particles \(`sub_82254B00`', 'blend', r'particle entry sets the blend code', ('hook', 'particles'), 'blend', why='LiveParticles'),
    Fam('post', r'post module', 'screen', r'post: depth test', ('phase', 'POST'), 'screen', pos=0, pos_re=r'flashes after the post',
        why='the post chain'),
    Fam('flashes', r'crowd flash manager', None, None, ('phase', 'FLASHES'), 'blend', pos=1, pos_re=r'flashes after the post',
        why='crowd camera flashes (DrawFlashesAfterPost, blended)'),
]

# A stage hook: the file and the gate line naming the Stage it draws at; the AttachFrameHooks member it is pushed as (None = pushed elsewhere).
HOOKS = {
    'crowd': ('src/renderer_scene/live_crowd.cpp', r'if \(stage != Stage::(\w+) \|\| !t\) return;', None),
    'particles': ('src/renderer_scene/live_particles.cpp', r'if \(stage != Stage::(\w+) \|\| !ctx\.frame', 'particles_'),
    'scene2d_early': ('src/renderer_scene/frame_hooks.h', r'stage == Stage::(\w+) && !early_\.empty\(\)', 'scene2d_'),
    'scene2d_late': ('src/renderer_scene/frame_hooks.h', r'stage == Stage::(\w+) && !rows_\.empty\(\)', 'scene2d_'),
}


# ------------------------------------------------------------------ readers
def disk_reader(root=ROOT):
    def read(rel):
        try:
            with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
                return f.read()
        except OSError:
            return None
    return read


def git_reader(ref, root=ROOT):
    def read(rel):
        p = subprocess.run(['git', 'show', f'{ref}:{rel}'], cwd=root, capture_output=True)
        return p.stdout.decode('utf-8', 'replace') if p.returncode == 0 else None
    return read


def code_only(text):
    """the text with // comments cut (enough for the anchors read here: none sits in a string)"""
    return '\n'.join(ln.split('//', 1)[0] for ln in text.split('\n'))


def body_of(text, head_re):
    """the lines of the function whose head matches head_re, to its first column-0 `}`"""
    lines = text.split('\n')
    for i, ln in enumerate(lines):
        if re.search(head_re, ln):
            for j in range(i + 1, len(lines)):
                if lines[j].startswith('}'):
                    return '\n'.join(lines[i:j + 1])
            return '\n'.join(lines[i:])
    return None


# ------------------------------------------------------------------ game side
def game_side(doc_text, fams):
    """-> ({fam: {'cats': [..], 'see', 'pos', 'mark', 'note'}}, problems)"""
    probs = qwhen.layout_problems(qwhen.parse_doc(doc_text))
    rows = []
    for line in doc_text.replace('\\|', '\x01').split('\n'):
        if not line.startswith('|'):
            continue
        cells = [c.strip().replace('\x01', '|') for c in line.strip().strip('|').split('|')]
        if len(cells) >= 8:
            cats = qwhen.parse_cats(cells[0].replace('`', ''))
            if cats:
                rows.append({'cats': cats, 'cells': cells})
    out = {}
    for f in fams:
        g = {'cats': [], 'see': None, 'pos': None, 'note': ''}
        if f.key is None:
            g['note'] = 'no doc row names it [U]'
            out[f.name] = g
            continue
        hit = [r for r in rows if re.search(f.key, r['cells'][1])]
        if not hit:
            g['note'] = f'doc text /{f.key}/ not found [U]'
            probs.append(f'{f.name}: its doc text /{f.key}/ is in no category row')
            out[f.name] = g
            continue
        cats = sorted({c for r in hit for c in r['cats']})
        g['cats'] = cats
        if f.see and f.see_re:
            if any(re.search(f.see_re, r['cells'][f.see_cell]) for r in hit):
                g['see'] = f.see
            else:
                g['note'] = f'state phrase /{f.see_re}/ not in the row [U]'
                probs.append(f'{f.name}: its state phrase /{f.see_re}/ is not in its row')
        if f.pos is not None and len(cats) == 1 and any(re.search(f.pos_re, '|'.join(r['cells'])) for r in hit):
            g['pos'] = f.pos
        out[f.name] = g
    return out, probs


def game_cmp(ga, gb):
    """-1 = a first, 1 = b first, None = [U]"""
    if not ga['cats'] or not gb['cats']:
        return None
    if max(ga['cats']) < min(gb['cats']):
        return -1
    if max(gb['cats']) < min(ga['cats']):
        return 1
    if len(ga['cats']) == 1 and ga['cats'] == gb['cats'] and ga['pos'] is not None and gb['pos'] is not None and ga['pos'] != gb['pos']:
        return -1 if ga['pos'] < gb['pos'] else 1
    return None


# ------------------------------------------------------------------ ours side
def ours_model(read, plant_parse=None):
    """-> (model, problems). model: phases [token], order_opaque bool, turns [name], turn_of {pred: turn, None: else}, classes [row],
    ranks [(pred 'a+b', rank)], rank_else (pred, a, b), place_ranks {rank} (ordered by place), late_cat (walked list 1's category: SeeRank's place
    term), hooks {id: stage}, push [member], late_bodies bool, gm_preds frozenset"""
    probs = []
    m = {'phases': [], 'order_opaque': False, 'turns': [], 'turn_of': {}, 'classes': [], 'ranks': [], 'rank_else': None, 'place_ranks': set(), 'late_cat': None,
         'hooks': {}, 'push': [], 'late_bodies': False, 'gm_preds': frozenset()}
    rec = read(RECORD)
    body = body_of(rec, r'^bool FrontEnd::Record\(') if rec else None
    if not body:
        probs.append(f'{RECORD}: FrontEnd::Record not found')
    else:
        for ln in body.split('\n'):
            code = ln.split('//', 1)[0]
            tok = None
            mm = re.search(r'\brun_stage\(Stage::(\w+)\)', code)
            if mm:
                tok = mm.group(1)
            elif re.search(r'for \(const Item& it : opaque\) emit\(it\)', code):
                tok = 'OPAQUE'
            elif re.search(r'for \(const Item& it : blended\) emit\(it\)', code):
                tok = 'BLENDED'
            elif re.search(r'post_pass_->Record\(', code):
                tok = 'POST'
            elif re.search(r'DrawFlashesAfterPost\(', code) and not (re.search(r'\bg_plant_', code) and not re.search(r'!g_plant_', code)):
                tok = 'FLASHES'
            if re.search(r'body_order::OrderOpaque\(opaque', code):
                m['order_opaque'] = True
            if tok and tok not in m['phases']:
                m['phases'].append(tok)
        for need in ('OPAQUE', 'BLENDED'):
            if need not in m['phases']:
                probs.append(f'FrontEnd::Record: the {need.lower()} loop not found')
        m['late_bodies'], m['late_probs'] = X.late_bodies(code_only(body), read(PLACE_ROWS))   # a problem only in the entrance scene (run)
    m['gm_preds'], gp = X.gm_preds(read(GM_STATE))
    probs += gp
    if m['gm_preds'] is None:
        m['gm_preds'] = frozenset()
    if m['order_opaque']:
        bo = read(BODY_ORDER)
        bo = code_only(bo) if bo else ''
        e = re.search(r'enum class Turn\s*:\s*\w+\s*\{([^}]*)\}', bo)
        t = re.search(r'Turn TurnOf\([^)]*\)\s*\{\s*return ([^;]*);', bo)
        if not e or not t:
            probs.append(f'{BODY_ORDER}: enum Turn / TurnOf not read: the opaque list taken in record order')
            m['order_opaque'] = False
        else:
            m['turns'] = [re.match(r'\s*(\w+)', x).group(1) for x in e.group(1).split(',') if x.strip()]
            for pred, turn in re.findall(r'(\w+)\s*\?\s*Turn::(\w+)', t.group(1)):
                m['turn_of'].setdefault(turn_meaning(bo, pred), turn)
            last = re.findall(r':\s*Turn::(\w+)\s*$', t.group(1).strip())
            m['turn_of'][None] = last[0] if last else None
    cl = read(CLASSES)
    cr = read(CROWS)
    names = re.findall(r'\{"(\w+)",\s*Family::', code_only(cr[cr.find('kCRows['):])) if cr and 'kCRows[' in cr else []
    tb = body_of(code_only(cl), r'Classes\(\)') if cl else None
    if not tb:
        probs.append(f'{CLASSES}: rf::Classes not found')
    else:
        for mm in re.finditer(r'c::kCRows\[(\d+)\]|\{"(\w+)",\s*Family::', tb):
            if mm.group(1) is not None:
                i = int(mm.group(1))
                if i < len(names):
                    m['classes'].append(names[i])
                else:
                    probs.append(f'{CLASSES}: kCRows[{i}] has no name in {CROWS}')
            else:
                m['classes'].append(mm.group(2))
    see = read(SEE)
    sr = body_of(code_only(see), r'inline int SeeRank\(') if see else None
    if not sr:
        probs.append(f'{SEE}: SeeRank not found')
    else:
        m['ranks'], m['rank_else'], rp = X.parse_see_rank(code_only(see), plant_parse)
        probs += rp
        sb = body_of(code_only(see), r'inline bool SeeThroughBefore\(') or ''
        m['place_ranks'], pp = X.place_ranks(sb, code_only(see))   # S111 REKEYORDER2: (ra == 1 || ra == kRankRekeyed)
        probs += pp
        if any(X.PLACE_LATE in p.split('+') for p, _ in m['ranks']):
            lt = read(DRAW_LISTS)
            m['late_cat'], lp = X.late_list_cat(code_only(lt) if lt else '')
            probs += lp
    for hid, (rel, gate, _member) in HOOKS.items():
        txt = read(rel)
        mm = re.search(gate, code_only(txt)) if txt else None
        if mm:
            m['hooks'][hid] = mm.group(1)
        else:
            probs.append(f'hook {hid}: the gate /{gate}/ not found in {rel}')
    at = read(ATTACH)
    ab = body_of(code_only(at), r'void AttachFrameHooks\(') if at else None
    m['push'] = re.findall(r'frame\.stage_hooks\.push_back\(h\.(\w+)\.get\(\)\)', ab) if ab else []
    if not ab:
        probs.append(f'{ATTACH}: AttachFrameHooks not found')
    return m, probs


# what a TurnOf predicate tests, in the facts a family declares: char_slot (its objects hold a character slot), char_effect (its material is a
# yCh_ effect), before_bodies. S111 FADECAW: TurnOf(bool body, ...) takes body_order.h BodyObject, whose real arm is `char_slot >= 0` and whose
# other arm is the planted twin g_plant_effect_body (skipped); FADEORDER's TurnOf(bool character_mesh, ...) tested the effect itself.
FACT_OF = {'character_mesh': 'char_effect', 'char_effect': 'char_effect', 'char_slot': 'char_slot', 'before_bodies': 'before_bodies'}


def turn_meaning(bo, pred):
    if pred == 'body':
        b = re.search(r'bool BodyObject\([^)]*\)\s*\{\s*return ([^;]*);', bo)
        if not b:
            return None
        expr = b.group(1).strip()
        p = re.fullmatch(r'g_plant_\w+\s*\?\s*(.+?)\s*:\s*(.+)', expr)
        if p:
            expr = p.group(2).strip()
        w = re.fullmatch(r'(\w+)(?:\s*>=\s*0)?', expr)
        return FACT_OF.get(w.group(1)) if w else None
    return FACT_OF.get(pred, pred)


def rank_of(m, preds):
    for pred, rank in m['ranks']:
        if X.rank_match(pred, preds):
            return rank
    if m['rank_else']:
        p, a, b = m['rank_else']
        return a if p in preds else b
    return None


def ours_key(m, f, scene='default'):
    """-> (key tuple | None, text). key = (phase index, sub order, record row index range or None)"""
    ours = f.ent_ours if scene == 'entrance' and f.ent_ours and m['late_bodies'] else f.ours
    f = _View(f, ours)
    kind = f.ours[0]
    ph = m['phases']
    if kind == 'opaque':
        _, rows, pred = f.ours
        if 'OPAQUE' not in ph or any(r not in m['classes'] for r in rows):
            return None, 'opaque list (anchor lost)'
        idx = [m['classes'].index(r) for r in rows]
        if m['order_opaque']:
            turn = next((m['turn_of'][x] for x in (pred or ()) if x in m['turn_of']), m['turn_of'].get(None))
            ti = m['turns'].index(turn) if turn in m['turns'] else None
            if ti is None:
                return None, 'opaque list (turn not read)'
            return (ph.index('OPAQUE'), ti, (min(idx), max(idx))), f'opaque list, turn {turn}, record row {"+".join(rows)}'
        return (ph.index('OPAQUE'), 0, (min(idx), max(idx))), f'opaque list, record row {"+".join(rows)} (#{min(idx)})'
    if kind == 'blended':
        _, rows, preds = f.ours
        if isinstance(preds, str):
            preds = m[preds]   # 'gm_preds': what GmNodeObjectFlags gives the object (gm_node_state.h)
        if f.name in m.get('place_late_fams', ()):
            preds = frozenset(preds) | {X.PLACE_LATE}   # S111 REKEYORDER2: placed in a walked list after 33 (run)
        if 'BLENDED' not in ph or any(r not in m['classes'] for r in rows):
            return None, 'blended list (anchor lost)'
        rk = rank_of(m, preds)
        if rk is None:
            return None, 'blended list (rank not read)'
        idx = [m['classes'].index(r) for r in rows]
        sub = 'G' if rk in m['place_ranks'] else (min(idx), max(idx))
        return (ph.index('BLENDED'), rk, sub), f'blended list rank {rk}' + (' (the game\'s list order, by place)' if sub == 'G' else f', record row {"+".join(rows)}')
    if kind == 'hook':
        st = m['hooks'].get(f.ours[1])
        if st is None or st not in ph:
            return None, f'hook {f.ours[1]} (anchor lost)'
        member = HOOKS[f.ours[1]][2]
        pi = m['push'].index(member) if member in m['push'] else None
        return (ph.index(st), 0, None if pi is None else (pi, pi)), f'hook {f.ours[1]} at Stage::{st}'
    if kind == 'phase':
        if f.ours[1] not in ph:
            return None, f'{f.ours[1]} (anchor lost)'
        return (ph.index(f.ours[1]), 0, (0, 0)), f.ours[1].lower() + ' (FrontEnd::Record)'
    return None, '?'


class _View:
    """a Fam seen with another `ours` (the entrance scene's)"""

    def __init__(self, f, ours):
        self._f, self.ours = f, ours

    def __getattr__(self, n):
        return getattr(self._f, n)


def ours_cmp(ka, kb):
    """-1 / 1, 0 = not ordered by our code (tie / overlap), 'G' = ours follows the game's list, None = not read"""
    if ka is None or kb is None:
        return None
    if ka[0] != kb[0]:
        return -1 if ka[0] < kb[0] else 1
    if ka[1] != kb[1]:
        return -1 if ka[1] < kb[1] else 1
    if ka[2] == 'G' and kb[2] == 'G':
        return 'G'
    ra, rb = ka[2], kb[2]
    if ra is None or rb is None or ra == 'G' or rb == 'G':
        return 0
    if ra[1] < rb[0]:
        return -1
    if rb[1] < ra[0]:
        return 1
    return 0


# ------------------------------------------------------------------ the join
def sens_of(f, g, plant=None):
    """True = blended / depth-write-off / a screen pass on either side; False = opaque with depth write on both; None = unknown"""
    if plant == 'opaque_only':
        return False
    if g['see'] in SENS or f.ours_see in SENS:
        return True
    if g['see'] == 'opaque' and f.ours_see == 'opaque':
        return False
    return None


def judge(gc, oc, sa, sb, plant=None):
    """-> (verdict, why)"""
    s = True if (sa or sb) else (False if sa is False and sb is False else None)
    if plant == 'any_swap':
        s = True
    if s is False:
        return 'OK', 'both opaque with depth write: the order changes no pixel'
    if oc == 'G':
        return ('OK', 'ours follows the game\'s list') if gc is not None else ('UNSETTLED', 'game order [U]')
    if gc is None:
        return 'UNSETTLED', 'game order [U]'
    if oc is None:
        return 'UNSETTLED', 'our order not read'
    if oc == 0:
        return 'UNSETTLED', 'our code does not order the two'
    if gc == oc:
        return 'OK', 'same order'
    if s is None:
        return 'UNSETTLED', 'opposite order, a state unknown'
    return 'DIFF', 'opposite order, a see-through family'


def run(read, doc_text, fams=None, plant=None, scene='default'):
    fams = fams or FAMILIES
    game, gprobs = game_side(doc_text, fams)
    m, oprobs = ours_model(read, 'skip_unread' if plant == 'skip_unread' else None)
    if scene == 'entrance' and plant != 'entrance_off':   # S111 DRAWORDER2: the re-key row's categories
        moved, rprobs = X.game_rekey(doc_text, fams)
        gprobs = gprobs + rprobs
        oprobs = oprobs + [f'(entrance scene) {p}' for p in m.get('late_probs', [])]
        for f in fams:
            if f.name in moved and game[f.name]['cats']:
                g = game[f.name]
                g['cats'] = sorted(set(moved[f.name]) | (set(g['cats']) if f.rekey_keep else set()))
                g['pos'] = None
                g['note'] = 'entrance re-key'
    elif scene == 'entrance':
        m['late_bodies'] = False
    # S111 REKEYORDER2: SeeRank's place term holds for a family whose categories (this scene's) are all at or after walked list 1 (41)
    lc = m.get('late_cat')
    m['place_late_fams'] = set() if lc is None or plant == 'place_late_off' else {f.name for f in fams if game[f.name]['cats'] and min(game[f.name]['cats']) >= lc}
    if plant == 'place_rank1_only':   # selftest twin: SeeThroughBefore read as ordering rank 1 only (the reader before S111 REKEYORDER2)
        m['place_ranks'] = m['place_ranks'] & {1}
    keys = {f.name: ours_key(m, f, scene) for f in fams}
    rows = []
    for i, a in enumerate(fams):
        for b in fams[i + 1:]:
            ga, gb = game[a.name], game[b.name]
            gc = game_cmp(ga, gb)
            oc = ours_cmp(keys[a.name][0], keys[b.name][0])
            sa, sb = sens_of(a, ga, plant), sens_of(b, gb, plant)
            v, why = judge(gc, oc, sa, sb, plant)
            gap = abs(min(gb['cats']) - min(ga['cats'])) if ga['cats'] and gb['cats'] else 0
            rows.append({'a': a.name, 'b': b.name, 'order': v, 'why': why, 'game': gc, 'ours': oc, 'sens': int(bool(sa)) + int(bool(sb)), 'gap': gap,
                         'game_a': ga, 'game_b': gb, 'ours_a': keys[a.name][1], 'ours_b': keys[b.name][1]})
    return {'rows': rows, 'game': game, 'ours': {f.name: keys[f.name][1] for f in fams}, 'model': m, 'problems': gprobs + oprobs}


def ranked(rows):
    return sorted(rows, key=lambda r: (-r['sens'], -r['gap'], r['a'], r['b']))


def cats_text(g):
    c = g['cats']
    if not c:
        return '[U]'
    runs, start = [], 0
    for i in range(1, len(c) + 1):
        if i == len(c) or c[i] != c[i - 1] + 1:
            runs.append(str(c[start]) if i - 1 == start else f'{c[start]}..{c[i - 1]}')
            start = i
    return ','.join(runs)


def first_text(r):
    def side(c, a, b):
        return '?' if c is None else ('-' if c == 0 else ('game list' if c == 'G' else (a if c == -1 else b)))
    return side(r['game'], r['a'], r['b']), side(r['ours'], r['a'], r['b'])


def main(argv=None):
    ap = argparse.ArgumentParser(prog='q draworder', description=__doc__.split('\n')[0])
    ap.add_argument('--flag', nargs='*', default=None, help='only these verdicts (DIFF / UNSETTLED / OK)')
    ap.add_argument('--fam', default=None, help='only pairs with this family')
    ap.add_argument('--ref', default=None, help='our code at this git ref (the doc stays the working tree\'s)')
    ap.add_argument('--families', action='store_true', help='each family: game category / state, our list / place')
    ap.add_argument('--arena', type=int, default=None, help='beside each DIFF row: can the two families overlap on screen in this arena (q overlap; unknown where a family has no file geometry)')
    ap.add_argument('--cams', type=int, default=48, help='with --arena: cameras sampled per DIFF pair (default 48; q overlap defaults to 144)')
    ap.add_argument('--scene', choices=SCENES, default='default', help='entrance = the entrance re-key sub_827FBFE0 (bodies 42, DRTR 43, own nodes 41 / 43)')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    with open(os.path.join(ROOT, DOC), encoding='utf-8') as fh:
        doc = fh.read()
    res = run(git_reader(a.ref) if a.ref else disk_reader(), doc, scene=a.scene)
    rows = res['rows']
    if a.fam:
        rows = [r for r in rows if a.fam in (r['a'], r['b'])]
    counts = {}
    for r in rows:
        counts[r['order']] = counts.get(r['order'], 0) + 1
    if a.flag:
        rows = [r for r in rows if r['order'] in a.flag]
    if a.arena is not None:
        import qoverlap   # S111 QOVERLAP: the screen-overlap test for a DIFF pair
        scene = None
        for r in rows:
            if r['order'] == 'DIFF':
                if scene is None:
                    scene = qoverlap.load_scene(a.arena)
                r['overlap'] = qoverlap.run(a.arena, r['a'], r['b'], a.cams, scene=scene)
    if a.json:
        print(json.dumps({'rows': rows, 'problems': res['problems'], 'ours': res['ours'], 'counts': counts}, indent=1, default=str))
        return 0
    for p in res['problems']:
        print(f'ANCHOR {p}')
    if a.families:
        for f in FAMILIES:
            g = res['game'][f.name]
            print(f'{f.name:15s} game {cats_text(g):6s} {g["see"] or "?":7s} pos {"-" if g["pos"] is None else g["pos"]}  ours {res["ours"][f.name]} ({f.ours_see})  {g["note"]}')
        print()
    order = {'DIFF': 0, 'UNSETTLED': 1, 'OK': 2}
    for r in sorted(rows, key=lambda r: order[r['order']]) if not a.flag or 'DIFF' not in a.flag else ranked(rows):
        gf, of = first_text(r)
        print(f"order {r['order']:9s} {r['a']} ({cats_text(r['game_a'])}) | {r['b']} ({cats_text(r['game_b'])})  game first: {gf}; ours first: {of}"
              f"  [{r['why']}]  ours: {r['ours_a']} | {r['ours_b']}"
              + (f"  overlaps: {r['overlap']['text']}" if 'overlap' in r else ''))
    print(f"draworder{' @' + a.ref if a.ref else ''}{' scene ' + a.scene if a.scene != 'default' else ''}: {len(FAMILIES)} families, {sum(counts.values())} pairs: "
          + ', '.join(f'{k} {counts.get(k, 0)}' for k in ('DIFF', 'UNSETTLED', 'OK')) + f"; anchors lost {len(res['problems'])}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
