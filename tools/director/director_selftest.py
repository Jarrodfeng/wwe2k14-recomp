"""S101 lane 161: offline checks of the situation director's first verbs (passive / momentum / stock / passive_cpu). No game runs.

    py -I tools/director/director_selftest.py             the unit test (src/situation_director_core.h COMPILED with clang++ against a fake game), its planted twins,
                                                          the parse rule against the strings the goto resolver builds, the goto ops / flags, the wiring pins,
                                                          the idle-pin detector on run 7092 (must flag) and run 5910 (must not)
    py -I tools/director/director_selftest.py --listing   pins of every address / offset the director calls against the generated listing of the game's functions
                                                          (reads generated\\ of the main checkout, never printed)

Exit 0 = all passed. A check that cannot read its input fails loudly.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'goto'))
import idle_pin_detector as D  # noqa: E402
import resolve  # noqa: E402

problems = []


def check(ok, msg):
    if not ok:
        problems.append(msg)
        print('  FAIL: ' + msg)


def find_clang():
    for c in (shutil.which('clang++'), r'C:\Program Files\LLVM\bin\clang++.exe', '/usr/bin/clang++'):
        if c and os.path.isfile(c):
            return c
    raise RuntimeError('no clang++ found (tools\\env.ps1 puts the project\'s LLVM on PATH): the director unit test cannot run')


def build(header_text, tmp, tag, verb_text=None, place_text=None):
    """Compiles director_test.cpp against `header_text` as src/situation_director_core.h (a copy under tmp: a planted twin never touches the tree).
    S110 OMGROUTE: the core includes src/situation_state_verb.h; `verb_text` (default: the shipped file) is its copy next to it."""
    inc = os.path.join(tmp, tag)
    os.makedirs(inc, exist_ok=True)
    with open(os.path.join(inc, 'situation_director_core.h'), 'w', encoding='utf-8') as f:
        f.write(header_text)
    with open(os.path.join(inc, 'situation_state_verb.h'), 'w', encoding='utf-8') as f:
        f.write(verb_text if verb_text is not None else open(os.path.join(REPO, 'src', 'situation_state_verb.h'), encoding='utf-8').read())
    with open(os.path.join(inc, 'situation_place_verb.h'), 'w', encoding='utf-8') as f:   # S110 PLACE: the core includes this one too
        f.write(place_text if place_text is not None else open(os.path.join(REPO, 'src', 'situation_place_verb.h'), encoding='utf-8').read())
    exe = os.path.join(inc, 'director_test.exe')
    r = subprocess.run([find_clang(), '-std=c++17', '-I', inc, os.path.join(HERE, 'director_test.cpp'), '-o', exe], capture_output=True, text=True, timeout=180)
    if r.returncode != 0 or not os.path.isfile(exe):
        raise RuntimeError('director_test did not compile (%s): %s' % (tag, (r.stdout + r.stderr)[-600:]))
    return exe


def run_test(exe):
    r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout


# ---- the planted twins: (name, regex pattern, replacement); each must change the header exactly once and make the test FAIL ----
PLANTS = [
    ('wrong_offset_cpu_word', r'kComActiveOff = 100;', 'kComActiveOff = 104;'),
    ('wrong_offset_meter', r'kMeterOff = 76;', 'kMeterOff = 80;'),
    ('wrong_offset_stock', r'kStockOff = 1928;', 'kStockOff = 1932;'),
    ('wrong_offset_chp_data', r'kChpDataOff = 24;', 'kChpDataOff = 28;'),
    ('missing_slot_table_guard', r'  if \(e\.U32\(ch \+ kSlotOff\) != uint32_t\(slot\)\) return [^\n]*\n', '\n'),
    ('missing_referee_guard', r'  if \(role == 4 \|\| role == 5 \|\| role == 6 \|\| role == 10\) return [^\n]*\n', '\n'),
    ('missing_match_guard', r'if \(!gate\.empty\(\)\) \{', 'if (false) {'),
    ('missing_decided_guard', r'if \(g\.g472\) return', 'if (false) return'),
    ('float_not_in_f1', r'e\.Call\(Fn::SetMeter, data, 0, fraction\);', 'e.Call(Fn::SetMeter, data, 0, 0.0);'),
    ('percent_not_scaled', r'double\(pct\) \* 0\.01', 'double(pct)'),
    ('no_readback_check_passive', r'if \(now != want\) return [^\n]*\n', '\n'),
    ('pad_call_present', r'  // B1 \(lane 161 review\): NO pad-record call\.', '  e.Call(Fn::SetComActive, ch, 0, 0.0);  // the planted B1 fault (a call on the character)\n  // B1 (lane 161 review): NO pad-record call.'),
    ('no_reset_before_the_bell', r'g\.seq != kSeqInGame \|\| !g\.clock_started\) \{\n    const bool latch', 'g.seq != kSeqInGame) {\n    const bool latch'),
    ('retry_never_dropped', r'return saw_match && seq != kSeqInGame;', 'return false;'),
    ('no_state_reset_outside_match', r'    st = State\{\};\n', '\n'),
    ('human_passive_allowed', r'if \(was == 0\) return "refused: slot " \+ std::to_string\(slot\) \+ " is not CPU-driven', 'if (false) return "refused: slot " + std::to_string(slot) + " is not CPU-driven'),
    ('off_ignores_changed_word', r'    if \(was != 0\) \{  // S1', '    if (false) {  // S1'),
    ('off_needs_full_gate', r'const std::string gate = restore \? \(g\.seq == kSeqInGame \? std::string\(\) : GateVerdict\(g\)\) : GateVerdict\(g\);', 'const std::string gate = GateVerdict(g);'),
    ('team_shared_momentum_not_refused', r'  if \(e\.Call\(Fn::TeamShared, 0, 0, 0\.0\) != 0\) return "refused: team-shared momentum', '  if (false) return "refused: team-shared momentum'),
    ('team_shared_stock_not_refused', r'  if \(e\.Call\(Fn::TeamShared, 0, 0, 0\.0\) != 0\) return "refused: team-shared stock', '  if (false) return "refused: team-shared stock'),
    ('off_form_too_wide', r'inline bool IsOffForm\(const Cmd& c\) \{ return \(c\.verb == Verb::Passive && !c\.on\)', 'inline bool IsOffForm(const Cmd& c) { return (c.verb == Verb::Passive)'),
    ('off_without_ownership', r'    if \(!\(sv\.on && sv\.ch == ch\)\) return "refused: slot "', '    if (false) return "refused: slot "'),
    ('auto_every_tick', r'  a\.applied = true;\n', '\n'),
    ('stock_wrong_direction', r'for \(int i = was; i < target; \+\+i\) e\.Call\(Fn::AddStock', 'for (int i = was; i < target; ++i) e.Call(Fn::RemoveStock'),
    # S105 T1 bleed twins (docs/research/s105/T1_blood.md): each must FAIL the unit test
    ('bleed_player_build_runs_it', r'if \(c\.verb == Verb::Bleed && !e\.Dev\(\)\) \{', 'if (false) {'),
    ('bleed_option_not_set_first', r'    e\.Put8\(kG \+ kBloodSwitchOff, 1\);\n', '\n'),
    ('bleed_level_off_by_one', r'uint64_t\(int64_t\(level - 1\)\)', 'uint64_t(int64_t(level))'),
    ('bleed_wrong_list_offset', r'kBleedListOff = 3736;', 'kBleedListOff = 3728;'),
    ('bleed_wrong_level_readback_offset', r'kBleedLevelOff = 1392;', 'kBleedLevelOff = 1396;'),
    ('bleed_bled_byte_not_set', r'  e\.Put8\(ch \+ kBledOff, 1\);\n', '\n'),
    ('bleed_no_pool_not_busy', r'if \(!e\.Ok\(x\) \|\| e\.U32\(x \+ kLiquidMapOff\) == 0\) \{', 'if (!e.Ok(x)) {'),
    ('bleed_lowering_not_refused', r'  if \(have > level\) \{', '  if (false) {'),
    # S105 SLO3 lightsout twins: each must FAIL the unit test
    ('lightsout_player_build_runs_it', r'if \(c\.verb == Verb::LightsOut && !e\.Dev\(\)\) \{', 'if (false) {'),
    ('lightsout_wrong_argument_order', r'e\.Call3\(Fn::DimSceneCtor, obj, ch, tgt\);', 'e.Call3(Fn::DimSceneCtor, obj, tgt, ch);'),
    ('lightsout_no_busy_check', r'if \(e\.U32\(mgr \+ kBrightOff\) != kBrightOne\) \{', 'if (false) {'),
    ('lightsout_wrong_alloc_size', r'kDimSceneSize = 136;', 'kDimSceneSize = 128;'),
    ('lightsout_wrong_target_offset', r'kTargetOff = 2672;', 'kTargetOff = 2668;'),
    ('bleed_no_readback_check', r'  if \(now != level\) \{\n    e\.Log\("director: " \+ tag \+ ": readback', '  if (false) {\n    e.Log("director: " + tag + ": readback'),
    ('all_includes_humans', r'if \(e\.U32\(comp \+ kComActiveOff\) == 0 && !\(st\.s\[slot\]\.on && st\.s\[slot\]\.ch == ch\)\) \{', 'if (false) {'),
    # S110 OMGROUTE `state SLOT NAME` twins (4th field 'verb' = the plant is applied to src/situation_state_verb.h, else to the core): each must FAIL the unit test
    ('state_player_build_runs_it', r'if \(c\.verb == Verb::State && !e\.Dev\(\)\) \{', 'if (false) {'),
    ('state_prefix_kept', r'if \(n\.size\(\) > 5 && n\.compare\(0, 5, "State"\) == 0\) n = n\.substr\(5\);', ''),
    ('state_not_routed_by_run', r'case Verb::State: return QueueState\(e, c\.slot, c\.name\);', 'case Verb::State: return "refused: no verb";'),
    ('state_wrong_machine_offset', r'kStateMachineOff = 3148;', 'kStateMachineOff = 3144;', 'verb'),
    ('state_wrong_descriptor_offset', r'kStateDescOff = 40;', 'kStateDescOff = 44;', 'verb'),
    ('state_wrong_records_offset', r'kStateRecsOff = 4;', 'kStateRecsOff = 8;', 'verb'),
    ('state_record_stride_4', r'kStateRecStride = 8;', 'kStateRecStride = 4;', 'verb'),
    ('state_name_at_record_0', r'kStateRecNameOff = 4;', 'kStateRecNameOff = 0;', 'verb'),
    ('state_record_0_and_last_findable', r'for \(uint32_t i = 1; i \+ 1 < n; \+\+i\) \{', 'for (uint32_t i = 0; i < n; ++i) {', 'verb'),
    ('state_already_in_it_queues', r'e\.U32\(cur \+ kStateCurIdxOff\) == uint32_t\(found\)\) \{', 'false) {', 'verb'),
    ('state_wrong_current_offset', r'kStateCurIdxOff = 12;', 'kStateCurIdxOff = 8;', 'verb'),
    ('state_wrong_current_object_offset', r'kStateCurOff = 12;', 'kStateCurOff = 16;', 'verb'),   # reviewer: [desc+12] (0x8266B57C), the other half of the already-in-it read
    ('state_unknown_name_called_anyway', r'if \(found < 0\) return refuse\(', 'if (false) return refuse(', 'verb'),
    ('state_prefix_match', r'if \(s == name\) \{', 'if (s.compare(0, name.size(), name) == 0) {', 'verb'),
    ('state_call_gets_the_name_not_the_record', r'e\.Call3\(Fn::QueueState, sm, namep, 0\);', 'e.Call3(Fn::QueueState, sm, recs + kStateRecStride * uint32_t(found), 0);', 'verb'),
    ('state_call_gets_the_character', r'e\.Call3\(Fn::QueueState, sm, namep, 0\);', 'e.Call3(Fn::QueueState, ch, namep, 0);', 'verb'),
    # S110 PLACE `place SLOT X Z [FACING]` twins (4th field 'place' = applied to src/situation_place_verb.h): each must FAIL the unit test
    ('place_player_build_runs_it', r'if \(c\.verb == Verb::Place && !e\.Dev\(\)\) \{', 'if (false) {'),
    ('place_not_routed_by_run', r'case Verb::Place: return PlaceWrestler\(e, c\.slot, c\.x, c\.z, c\.has_facing, c\.deg\);', 'case Verb::Place: return "refused: no verb";'),
    ('place_parse_no_bound_on_x', r'SignedDecimal\(w\[2\], kPlaceCoordMaxParse, out\.x\)', 'SignedDecimal(w[2], 1e9f, out.x)'),
    ('place_place_byte_not_restored', r'  e\.Put8\(ch \+ kPlaceOff, place\);\n  // 6\.', '  // 6.', 'place'),
    ('place_facing_skipped', r'if \(has_facing\) e\.CallF\(Fn::SetYaw, ch, ', 'if (false) e.CallF(Fn::SetYaw, ch, ', 'place'),
    ('place_facing_degrees_not_converted', r'double\(deg\) \* \(3\.14159265358979323846 / 180\.0\)\);\n  // 3\.', 'double(deg));\n  // 3.', 'place'),
    ('place_ctrl_angle_not_written', r'  e\.Put32\(ctrl \+ kCtrlAngOff, e\.U32\(ch \+ kYawOff\)\);\n', '\n', 'place'),
    ('place_old_rotation_not_copied', r'  for \(uint32_t i = 0; i < 16; i \+= 4\) e\.Put32\(ch \+ kOldRotOff', '  for (uint32_t i = 0; i < 0; i += 4) e.Put32(ch + kOldRotOff', 'place'),
    ('place_warp_not_called', r'  e\.CallF3\(Fn::Warp, ch, double\(x\), y, double\(z\)\);\n', '\n', 'place'),
    ('place_warp_y_not_passed', r'e\.CallF3\(Fn::Warp, ch, double\(x\), y, double\(z\)\);', 'e.CallF3(Fn::Warp, ch, double(x), 0.0, double(z));', 'place'),
    ('place_floor_before_place_byte', r'  e\.Put8\(ch \+ kPlaceOff, place\);\n  const double y', '  const double y', 'place'),
    ('place_x_not_stored_before_place_call', r'  e\.Put32\(ch \+ kPosOff, FloatBits\(x\)\);\n', '\n', 'place'),
    ('place_root_node_not_stored', r'    e\.Put32\(root \+ 0, FloatBits\(x\)\);\n', '\n', 'place'),
    ('place_partner_not_refused', r'  if \(e\.U32\(ch \+ kPartnerOff\) != 0\) return refuse', '  if (false) return refuse', 'place'),
    ('place_holder_not_refused', r'e\.U32\(other \+ kPartnerOff\) == ch\) return refuse', 'false) return refuse', 'place'),
    ('place_kind_not_refused', r'if \(e\.U32\(ch \+ kKindOff\) != kWrestlerKind\) return refuse', 'if (false) return refuse', 'place'),
    ('place_actor_not_refused', r'if \(actor_idx >= 0 && actor_idx < kActorCount && e\.U32\(kActorTable \+ 4u \* uint32_t\(actor_idx\)\) != 0\) return refuse', 'if (false) return refuse', 'place'),
    ('place_actor_index_unbounded', r'if \(actor_idx >= 0 && actor_idx < kActorCount && ', 'if (', 'place'),   # reviewer: -1 wraps to the word before the table
    ('place_special_not_refused', r'if \(e\.U32\(ch \+ kSpecialOff\) != 0\) return refuse', 'if (false) return refuse', 'place'),
    ('place_no_controller_not_refused', r'if \(!e\.Ok\(ctrl\)\) return refuse', 'if (false) return refuse', 'place'),
    ('place_wrong_controller_offset', r'kCtrlOff = 3096;', 'kCtrlOff = 3092;', 'place'),
    ('place_wrong_pos_offset', r'kPosOff = 160;', 'kPosOff = 164;', 'place'),
    ('place_wrong_yaw_offset', r'kYawOff = 148;', 'kYawOff = 152;', 'place'),
    ('place_wrong_place_offset', r'kPlaceOff = 70;', 'kPlaceOff = 71;', 'place'),
    ('place_wrong_ctrl_angle_offset', r'kCtrlAngOff = 32;', 'kCtrlAngOff = 36;', 'place'),
]


def unit_tests(tmp):
    header = open(os.path.join(REPO, 'src', 'situation_director_core.h'), encoding='utf-8').read()
    code, out = run_test(build(header, tmp, 'honest'))
    m = re.search(r'SUMMARY (\d+) passed, (\d+) failed', out)
    check(code == 0 and m and m.group(2) == '0', 'the unit test fails on the shipped header:\n' + out[-800:])
    n = int(m.group(1)) if m else 0
    print('  unit test: %s' % (m.group(0) if m else 'no summary'))
    caught = 0
    verb_src = open(os.path.join(REPO, 'src', 'situation_state_verb.h'), encoding='utf-8').read()
    place_src = open(os.path.join(REPO, 'src', 'situation_place_verb.h'), encoding='utf-8').read()
    for plant in PLANTS:
        name, pat, rep = plant[:3]
        which = plant[3] if len(plant) > 3 else 'core'
        target = {'core': header, 'verb': verb_src, 'place': place_src}[which]
        hits = len(re.findall(pat, target))
        if hits != 1:
            check(False, 'plant %s: the pattern matches %d times in the header (must be exactly 1: the plant is stale)' % (name, hits))
            continue
        twin = re.sub(pat, lambda _m: rep, target, count=1)
        try:
            if which == 'verb':
                code, out = run_test(build(header, tmp, 'plant_' + name, twin))
            elif which == 'place':
                code, out = run_test(build(header, tmp, 'plant_' + name, None, twin))
            else:
                code, out = run_test(build(twin, tmp, 'plant_' + name))
        except RuntimeError as x:
            check(False, 'plant %s does not compile: %s' % (name, x))
            continue
        failed = [ln.split()[1] for ln in out.splitlines() if ln.startswith('FAIL')]
        if code == 0:
            check(False, 'plant %s was NOT caught by the unit test' % name)
        else:
            caught += 1
            print('  plant %-28s caught by: %s' % (name, ', '.join(failed[:3]) or 'a crash (exit %d)' % code))
    print('  planted twins caught: %d of %d' % (caught, len(PLANTS)))
    return n


def parse_rule(tmp):
    """The strings the goto resolver builds (director_arg) and the input-script spelling are accepted by the game's parse rule; garbage is not."""
    exe = build(open(os.path.join(REPO, 'src', 'situation_director_core.h'), encoding='utf-8').read(), tmp, 'parse')
    steps = [{'op': 'passive', 'slot': 1, 'on': True}, {'op': 'passive', 'slot': 'all', 'on': False}, {'op': 'momentum', 'slot': 2, 'pct': 100},
             {'op': 'momentum', 'slot': 0, 'pct': 0}, {'op': 'stock', 'slot': 3, 'n': 9}, {'op': 'passive_cpu', 'on': True}, {'op': 'passive_cpu', 'on': False}]
    texts = [resolve.director_arg(s) for s in steps]
    r = subprocess.run([exe, 'parse'], input='\n'.join(texts) + '\n', capture_output=True, text=True, timeout=30)
    got = r.stdout.splitlines()
    check(len(got) == len(texts) and all(g.startswith('OK') for g in got), 'the resolver\'s director strings are not all accepted by the game parse: %s -> %s' % (texts, got))
    for s, t in zip(steps, texts):   # the round trip text -> step -> text
        check(resolve.director_arg(resolve.director_step_from_text(t)) == t, 'round trip of %r' % t)
    # S105 T1: the script line `bleed 0 3` is accepted by the parse the input script and the live channel share; the bad forms are not
    r = subprocess.run([exe, 'parse'], input='bleed 0 3\nbleed 8 1\n', capture_output=True, text=True, timeout=30)
    check(len(r.stdout.splitlines()) == 2 and all(g.startswith('OK') for g in r.stdout.splitlines()), 'bleed script lines are not accepted by the game parse: %s' % r.stdout.split())
    bad = ['passive', 'passive 9 on', 'momentum 1 101', 'stock 1 10', 'passive_cpu', 'dance 1 2']
    r = subprocess.run([exe, 'parse'], input='\n'.join(bad) + '\n', capture_output=True, text=True, timeout=30)
    check(r.stdout.split() == ['ERR'] * len(bad), 'bad director strings are not all refused by the game parse: %s' % r.stdout.split())
    for t in bad:
        try:
            resolve.director_step_from_text(t) if t.split()[0] in resolve.DIRECTOR_OPS else (_ for _ in ()).throw(resolve.AddressError('x'))
            if t not in ('passive 9 on', 'momentum 1 101', 'stock 1 10'):   # these three parse as a step; the game refuses them (checked above)
                check(False, 'the resolver accepted %r' % t)
        except resolve.AddressError:
            pass


def goto_ops():
    plan = resolve.Plan('x', 'plan', steps=[{'op': 'nav', 'target': 'mode:wm30'}, {'op': 'wm30_arm', 'n': 3, 'code': 0, 'era': 0}, {'op': 'expect_log', 'text': 'jump', 'timeout_s': 9}])
    resolve.add_director(plan, ['momentum 0 100', 'passive all on'], passive_cpu=True)
    ops = [s['op'] for s in plan.steps]
    check(ops == ['nav', 'passive_cpu', 'wm30_arm', 'expect_log', 'momentum', 'passive'], 'plan ops are %s' % ops)
    for s in plan.steps:
        check(bool(resolve.describe_step(s)), 'describe_step of %s' % s['op'])
    plan2 = resolve.Plan('x', 'plan', steps=[{'op': 'nav', 'target': 't'}])
    resolve.add_director(plan2, passive_cpu=True)
    check([s['op'] for s in plan2.steps] == ['nav', 'passive_cpu'], 'a plan with no match step gets the latch at its end: %s' % plan2.steps)
    plan3 = resolve.Plan('x', 'error', reason='no')
    resolve.add_director(plan3, ['stock 0 1'], True)
    check(plan3.steps == [], 'a plan that is not a plan is left alone')
    try:
        resolve.add_director(resolve.Plan('x', 'plan'), ['stock 0'])
        check(False, 'a bad --director text was accepted')
    except resolve.AddressError:
        pass


def rd(rel):
    return open(os.path.join(REPO, *rel.split('/')), encoding='utf-8').read()


def wiring():
    live, sd, app, inp, glue, core = rd('src/live_control.h'), rd('src/scene_director.h'), rd('src/wwe2k14_app.h'), rd('src/input_script.h'), rd('src/situation_director.h'), rd('src/situation_director_core.h')
    for ok, msg in [
        ('g_director_live' in live and 'director_cmd' in live and '"director"' in live, 'live_control.h does not route !director'),
        ('g_situation_tick(ctx, base)' in sd and '(*g_situation_tick)(PPCContext&, uint8_t*) = nullptr' in sd, 'scene_director.h does not call the director tick'),
        ('uint32_t withf(' in rd('src/guest_call.h') and 'ctx.f1.f64 = f1;' in rd('src/guest_call.h') and '#include "guest_call.h"' in sd,
         'guest_call.h Caller has no float call (withf / ctx.f1), or scene_director.h does not include it (EXEB-B1 moved Caller there)'),
        ('#include "situation_director.h"' in app, 'wwe2k14_app.h does not include situation_director.h'),
        ('Op::kDirector' in inp and 'situation::Submit(step.text)' in inp and 'situation::IsVerb(cmd)' in inp, 'input_script.h does not carry the director verbs'),
        ('call.withf(sub_825F1F10, {r3}, f1)' in glue, 'the momentum setter is not called with f1'),
        ('call(sub_823C8FB8, {r3, r4})' in glue and 'sub_8241DAB0' not in glue.replace('B1', '') .replace('lane 161 review B1', '') and 'call(sub_82C31CB0, {r3, r4})' in glue and 'call(sub_825E7168, {})' in glue, 'a leaf call in the glue changed'),
        ('call(sub_825F4510, {r3})' in glue and 'call(sub_825F1DF0, {r3})' in glue, 'a stock call in the glue changed'),
        ('g_director_live = &LiveCommand' in glue and 'g_situation_tick = &Tick' in glue, 'the glue does not register itself'),
        ('GateVerdict(g)' in core and 'const Gate g = e.GetGate();' in core, 'Run does not test the match gate'),
        ('src/situation_director_core.h' in rd('tools/director/director_selftest.py') or True, ''),
    ]:
        check(ok, msg)
    # S105 SLO3: the lightsout glue (the factory's own two calls, dev-gated at parse and run time)
    check('call(sub_82250050, {r3})' in glue and 'call(sub_8275DF98, {r3, r4, r5})' in glue, 'the lightsout glue (alloc / ctor calls) changed')
    check('Verb::LightsOut && !wwe2k14::IsDevBuild()' in inp, 'input_script.h does not refuse lightsout at parse in a player build')
    # S110 OMGROUTE: the state verb's glue (the game's own queue, r3 = state machine, r4 = name), the parse refusal in a player build, the core includes the new file
    check('Fn::QueueState) return call(sub_8266B550, {r3, r4})' in glue, 'the state verb glue (sub_8266B550 with r3 / r4) changed')
    check('Verb::State && !wwe2k14::IsDevBuild()' in inp, 'input_script.h does not refuse state at parse in a player build')
    check('#include "situation_state_verb.h"' in core and 'return QueueState(e, c.slot, c.name)' in core, 'the core does not include / route the state verb')
    # S110 PLACE: the float callers, the four glue mappings, the parse refusal in a player build, the core routes and includes the verb
    gc = rd('src/guest_call.h')
    check('double retf(' in gc and 'const double f1 = ctx.f1.f64;\n    ctx = saved;' in gc and
          'uint32_t withf3(' in gc and 'ctx.f3.f64 = f3;' in gc, 'guest_call.h has no retf (f1 read before the restore) / withf3 (f1..f3)')
    check('call.withf(sub_822FB578, {r3}, f1)' in glue and 'call.retf(sub_82612BF8, {r3, r4})' in glue and 'call.withf3(sub_829C0EE8, {r3}, f1, f2, f3)' in glue and 'call(sub_82612E88, {r3})' in glue,
          'the place verb glue (yaw / floor / warp / place calls) changed')
    check('Verb::Place && !wwe2k14::IsDevBuild()' in inp, 'input_script.h does not refuse place at parse in a player build')
    check('#include "situation_place_verb.h"' in core and 'return PlaceWrestler(e, c.slot, c.x, c.z, c.has_facing, c.deg)' in core, 'the core does not include / route the place verb')
    # nothing runs unless asked: the tick is free when idle (the latch is off, the queue empty, nothing pending)
    check('if (g_state.passive_cpu || g_auto.applied)' in glue and 'TakeQueued(text)' in glue and 'g_pending.exchange(false)' in glue, 'the tick is not a no-op when idle')
    check('Housekeeping(g_state, g)' in glue and 'seq != scenario::kSeqInGame' in glue, 'the glue does not reset the state outside the in-game sequence on every tick (S1)')
    check('replay::verbs::Playing() && !IsOffForm(c)' in glue and 'g_record_event(scenario::Frame(), "director " + text)' in glue and 'g_event_handlers.push_back(&OnReplayEvent)' in glue, 'the replay contract is not wired (S3)')
    check('DropsRetry(r.saw_match, seq)' in glue and 'playback diverges' in glue, 'F3 / F4 not wired in the glue')
    check('NOT DONE after' in glue and 'kScriptBusyFrames' in glue, 'a script verb can still be dropped silently (S6)')
    check('Fn::PadRecord' not in core and 'Fn::PadRecord' not in glue and 'PadRecord' not in glue, 'a pad-record call is back in the director (B1)')
    # the script verbs: a line's text is checked at parse time (a bad line fails the script before the game starts)
    # S105 T1: the bleed verb reaches the queue through the same script op, and the glue is dev-gated and calls the setter with r3 / r4 / r5
    check('bleed SLOT LEVEL' in inp and 'bleed' in core and '"bleed"' in core, 'input_script.h / the core do not carry the bleed verb')
    check('call(sub_825A1F28, {r3, r4, r5})' in glue and 'wwe2k14::IsDevBuild()' in glue and '"dev_build.h"' in glue, 'the bleed glue (setter call with r5 / IsDevBuild) changed')
    check('e.Dev()' in core and core.index('e.Dev()') < core.index('const Gate g = e.GetGate();', core.index('std::string Run(')), 'the dev gate is not before the match gate in Run')
    check('director::Parse' not in inp, 'input_script.h still names the old namespace (wwe2k14::director is the match director)')


def detector():
    fx = os.path.join(HERE, 'fixtures')
    r_j3 = D.analyse(D.read_events(os.path.join(fx, 'run7092_match.jsonl'))[0])
    r_ok = D.analyse(D.read_events(os.path.join(fx, 'run5910_match.jsonl'))[0])
    check(len(r_j3) == 1 and r_j3[0]['verdict'] == 'no_defender' and r_j3[0]['pinner'] == 1 and r_j3[0]['loser'] == 0, 'fixture run 7092 (J3) is not no_defender as the CPU pinning an idle slot 0: %s' % r_j3)
    check(len(r_ok) == 1 and r_ok[0]['verdict'] == 'clean', 'fixture run 5910 is flagged: %s' % r_ok)
    # a passive-by-director pin is reported expected, not a flag; a move by the loser before the pin is a player
    ev = D.read_events(os.path.join(fx, 'run7092_match.jsonl'))[0]
    check(D.analyse(ev, [{'slot': 0, 'on': True, 'after_frame': 19000, 'line': 1}])[0]['verdict'] == 'passive', 'a passive-by-director pin is not reported as passive')
    check(D.analyse(ev, [{'slot': 0, 'on': True, 'after_frame': 19200, 'line': 1}])[0]['verdict'] == 'no_defender', 'a passive line AFTER the pin must not explain it')
    check(D.analyse(ev, [{'slot': 0, 'on': True, 'after_frame': 18000, 'line': 1}, {'slot': 0, 'on': False, 'after_frame': 18500, 'line': 2}])[0]['verdict'] == 'no_defender', 'a passive off before the pin must end the explanation')
    check(D.analyse(ev, [{'slot': 0, 'on': True, 'after_frame': None, 'line': 1}])[0]['verdict'] == 'passive', 'a passive line before any bus line (frame unknown) is before the pin')
    mv = ev + [{'f': 18000, 'kind': 'move', 'slot': 0, 'class': 'strike'}]
    check(D.analyse(mv)[0]['verdict'] == 'clean', 'a move by the loser before the pin is not accepted as a sign of a player')
    mv_far = ev + [{'f': 1000, 'kind': 'move', 'slot': 0, 'class': 'strike'}]
    check(D.analyse(mv_far)[0]['verdict'] == 'no_defender', 'a move 18000 frames before the pin must be outside the window')
    check(D.analyse([e for e in ev if e.get('what') != 'finish' or e.get('finish_type') != 1])[0]['verdict'] == 'unfinished', 'a pin with no finish is not reported unfinished')
    # the real run logs, when they still exist (the disk tidies itself after 24 h: the fixtures above carry the same lines)
    real = glob.glob(r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs\events_7092.jsonl')
    real5910 = glob.glob(r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\logs\events_5910.jsonl')
    for path, want in [(p, 'no_defender') for p in real] + [(p, 'clean') for p in real5910]:
        res = D.analyse(D.read_events(path)[0])
        check(any(x['verdict'] == want for x in res), 'real %s: no %s verdict: %s' % (os.path.basename(path), want, [x['verdict'] for x in res]))
        print('  real %s: %s' % (os.path.basename(path), [x['verdict'] for x in res]))
    if not real or not real5910:
        print('  (real event files %s gone: the committed fixtures were used)' % ('7092' if not real else '5910'))
    # the command line: exit 3 flagged, 0 clean
    for fixture, code in (('run7092_match.jsonl', 3), ('run5910_match.jsonl', 0)):
        r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'idle_pin_detector.py'), os.path.join(fx, fixture)], capture_output=True, text=True, timeout=30)
        check(r.returncode == code, 'detector exit for %s is %d, not %d: %s' % (fixture, r.returncode, code, r.stdout[-200:]))


LISTING = {   # address: [text that must be in the listing]
    '827349E8': ['lwz r3,3088(r3)', 'cntlzw r11,r31', 'bl 0x823c8fb8'],
    '823C8FB8': ['stw r4,100(r3)'],
    '82735298': ['lwz r3,3728(r3)', 'lis r4,17224', 'ori r4,r4,20512', 'bl 0x82c31cb0', 'lwz r3,24(r3)', 'lfs f0,15500(r10)', 'bl 0x825f1f10'],
    '827354A0': ['lwz r30,24(r3)', 'bl 0x825f4510', 'bl 0x825f1df0'],
    '825F1F10': ['fmr f31,f1', 'stfs f31,76(r31)', 'lwz r10,3720(r11)'],
    '825F4510': ['lwz r11,32(r31)', 'addi r11,r11,1816', 'lwz r10,112(r11)', 'stw r10,112(r11)'],
    '825F1DF0': ['addi r11,r11,1816', 'lwz r10,112(r11)', 'addi r10,r10,-1', 'stw r10,112(r11)'],
    '825F1C50': ['lwz r3,1928(r11)'],
    '8241DAB0': ['lwz r3,132(r3)', 'stw r30,1888(r31)'],
    '823AE0B8': ['cmpwi cr6,r3,9', 'lwzx r3,r10,r9'],
    '825A0B50': ['lwz r3,3088(r31)', 'lwz r3,100(r3)'],
    '825A0F28': ['lwz r3,3736(r3)', 'lis r4,17218', 'ori r4,r4,17475', 'lwz r3,24(r3)', 'stb r4,1392(r31)'],
    '825C19B0': ['lwz r3,36(r3)'],            # S105 T1 (note section 4; the 'CBDC' constant is read from 0x825A0F4C by the probe)
    '825A1F28': ['15229(r3)', 'li r30,-1'],
    # S110 OMGROUTE `state SLOT NAME` (note: docs/research/s110/OMGROUTE.md section 3): yaseChgState hands [char+3148] and the name to sub_8266B550; that one finds the
    # index with sub_826AC388 (desc = [sm+40], records [desc+4], count [desc]) and calls the record's constructor at index * 8 (+0)
    '826D9080': ['lwz r3,3148(r11)', 'bl 0x8266b550', 'cmplwi cr6,r10,97', 'cmplwi cr6,r10,116', 'cmplwi cr6,r10,101', 'addi r4,r4,5'],
    '8266B550': ['lwz r3,40(r3)', 'bl 0x826ac388', 'lwz r11,40(r30)', 'lwz r10,4(r11)', 'lwz r11,0(r11)', 'rlwinm r11,r29,3,0,28', 'lwzx r31,r11,r10',
                 'lwz r10,12(r11)', 'lwz r10,12(r10)', 'cmplw cr6,r10,r3'],   # reviewer: the already-in-it read [[desc+12]+12] == the found index (0x8266B57C-0x8266B590)
    '826AC388': ['lwz r4,4(r11)', 'lwz r11,0(r11)', 'bl 0x826ac270'],
    # S110 PLACE (note: docs/research/s110/PLACE.md "Sites the build will touch"): the all-copies warp, the floor height, the match-start placement's own use of the two
    # helpers, the controller's per-frame copy, the root applier's controller read, the yaw setter
    '829C0EE8': ['stfs f1,160(r3)', 'std r11,192(r3)', 'stfs f0,0(r9)', 'lwz r11,3096(r3)', 'std r10,48(r11)', 'std r8,56(r11)', 'bl 0x823a6450', 'std r11,176(r3)', 'stb r11,70(r31)'],
    '82612BF8': ['cmplwi cr6,r11,15', 'lbz r4,70(r3)'],
    '8273B790': ['bl 0x82612e88', 'stb r3,70(r31)', 'li r4,255', 'bl 0x82612bf8', 'stfs f1,164(r31)', 'lwz r30,3096(r31)', 'std r9,48(r30)', 'stfs f0,32(r30)'],
    '82410AB8': ['stfs f0,32(r31)', 'std r8,48(r31)', 'std r7,56(r31)'],
    '823EA6E0': ['lwz r3,3096(r11)'],
    '822FB578': ['stfs f1,148(r3)'],
    '823CB3F0': ['lwz r11,1340(r3)', 'cmpwi cr6,r11,-1', 'cmpwi cr6,r11,72', 'addi r8,r10,19264'],   # reviewer: the actor table is read only for 0 <= index < 72 (0x836F4B40 = lis -31889 + 19264)
}


def listing():
    main = os.environ.get('RDR_MAIN_CHECKOUT') or (REPO if os.path.isdir(os.path.join(REPO, 'generated', 'default')) else os.path.abspath(os.path.join(REPO, '..', '..', '..')))
    files = glob.glob(os.path.join(main, 'generated', 'default', 'wwe2k14_recomp.*.cpp'))
    if not files:
        check(False, '--listing: no generated code under %s (this check needs the player\'s own recompiled output)' % main)
        return
    for addr, needles in LISTING.items():
        out = subprocess.run([sys.executable, '-I', os.path.join(main, 'tools', 'analysis', 'list_function.py'), addr], cwd=main, capture_output=True, text=True, timeout=120).stdout
        low = out.lower()
        if 'not found' in low:
            check(False, '--listing: sub_%s not in the generated code' % addr)
            continue
        for n in needles:
            check(n.lower() in low, '--listing: sub_%s no longer has "%s"' % (addr, n))
    # the numbers in the core, against the listing's
    core = rd('src/situation_director_core.h')
    for name, val in (('kComOff', 3088), ('kComActiveOff', 100), ('kListOff', 3728), ('kChpDataOff', 24), ('kMeterOff', 76), ('kStockOff', 1928), ('kChpTag', 0x43485020),
                      ('kBleedListOff', 3736), ('kCbdcTag', 0x43424443), ('kLiquidOff', 24), ('kLiquidMapOff', 36), ('kBleedLevelOff', 1392), ('kBledOff', 1398),
                      ('kG', 0x8376AAF4), ('kBloodSwitchOff', 15229)):
        m = re.search(r'%s = (0x[0-9A-Fa-f]+|\d+);' % name, core)
        check(bool(m) and int(m.group(1), 0) == val, 'core %s is not %s' % (name, val))
    check(0x4348 == 17224 and (0x4348 << 16 | 20512) == 0x43485020, 'the CHP tag arithmetic (lis 17224 / ori 20512)')
    check(0x4342 == 17218 and (0x4342 << 16 | 17475) == 0x43424443, 'the CBDC tag arithmetic (lis 17218 / ori 17475)')
    verb = rd('src/situation_state_verb.h')   # S110 OMGROUTE: the verb's numbers against the pins above (0x826D9120 +3148; 0x8266B560 +40; 0x8266B594 +4; 0x8266B5AC rlwinm 3 = stride 8)
    for name, val in (('kStateMachineOff', 3148), ('kStateDescOff', 40), ('kStateRecsOff', 4), ('kStateRecStride', 8), ('kStateRecNameOff', 4), ('kStateCurOff', 12), ('kStateCurIdxOff', 12)):
        m = re.search(r'%s = (0x[0-9A-Fa-f]+|\d+);' % name, verb)
        check(bool(m) and int(m.group(1), 0) == val, 'state verb %s is not %s' % (name, val))
    place = rd('src/situation_place_verb.h')   # S110 PLACE: the verb's numbers against the pins above
    for name, val in (('kPosOff', 160), ('kYOff', 164), ('kZOff', 168), ('kYawOff', 148), ('kRotOff', 144), ('kOldRotOff', 208), ('kPlaceOff', 70), ('kKindOff', 24), ('kCtrlOff', 3096),
                      ('kCtrlAngOff', 32), ('kCtrlPosOff', 48), ('kPartnerOff', 836), ('kActorTable', 0x836F4B40), ('kActorIdxOff', 1340), ('kActorCount', 72), ('kSpecialOff', 1920), ('kRootOff', 176)):
        m = re.search(r'%s = (0x[0-9A-Fa-f]+|\d+);' % name, place)
        check(bool(m) and int(m.group(1), 0) == val, 'place verb %s is not %s' % (name, val))
    print('  listing pins: %d functions, %d texts' % (len(LISTING), sum(len(v) for v in LISTING.values())))


def main(argv):
    tmp = tempfile.mkdtemp(prefix='l161_')
    try:
        if '--listing' in argv:
            listing()
        else:
            n = unit_tests(tmp)
            parse_rule(tmp)
            goto_ops()
            wiring()
            detector()
            print('  unit checks run: %d' % n)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if problems:
        print('FAILED: %d problem(s)' % len(problems))
        return 1
    print('PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
