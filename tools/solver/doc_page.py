r"""Renders docs\renderer\CONSTANTS.md from the solver's result: counts and names only (no value from the game's files, CLAUDE.md hard rule 1)."""
import collections
import time


def _scene_table(scenes):
    rows = ['| scene | frames (read / in zip) | draws | joined | joined, several meshes fit | unjoined | no file source by design |',
            '|---|---|---|---|---|---|---|']
    for s in scenes:
        rows.append(f"| {s['scene']} | {s['frames_read']} / {s['frames_total']} | {s['draws']} | {s['joined']} | {s['joined_ambiguous']} | "
                    f"{sum(s['unjoined'].values())} | {sum(s['no_file_source'].values())} |")
    tot = lambda k: sum(s[k] for s in scenes)
    rows.append(f"| **all** | {tot('frames_read')} | {tot('draws')} | {tot('joined')} | {tot('joined_ambiguous')} | "
                f"{sum(sum(s['unjoined'].values()) for s in scenes)} | {sum(sum(s['no_file_source'].values()) for s in scenes)} |")
    return '\n'.join(rows)


def _reasons(scenes, key):
    c = collections.Counter()
    for s in scenes:
        c.update(s[key])
    return '\n'.join(f'| {r} | {n} |' for r, n in c.most_common())


def render(res, mesh_count):
    scenes = res['scenes']
    summ = res['summary']
    effects = res['effects']
    now = time.strftime('%Y-%m-%d %H:%M')
    out = []
    w = out.append
    w('# Effect constants: where each value comes from (Session 101, lane 16, arc tool 6)')
    w('')
    w(f'Generated {now} by `py -I tools\\solver\\solve.py --doc docs\\renderer\\CONSTANTS.md` over the six Check A case zips '
      f"(`out\\s101\\runs\\*\\case_*.zip`), {res['frames_per_zip']} frames per zip, in {res['seconds']} s, 0 game runs. "
      'This page holds counts and names only; the values are in `out\\s101\\solver\\constants.json` (local, game-derived). Arc: `RENDERER_ARC.md` '
      'section 4 tool 6 and step B3: what stays RUNTIME is the exact list of game hooks Phase C needs (arc P2).')
    w('')
    w('## What it answers')
    w('For every constant of every effect a captured draw used (`field` in `src\\renderer\\shaders\\fx\\<effect>.fx.json`: register, count, stage):')
    w('')
    w('| class | meaning | evidence |')
    w('|---|---|---|')
    w('| **FILE** | the value is the named parameter of the mesh in the model file (`g_*`, the drawables atlas) | equal (float tolerance 1e-5) in **every** joined draw it was compared in |')
    w('| **SCENE** (declared, untested) | a camera / light / light-set / fog / time quantity | declared by the constant\'s own name (`SCENE_NAMES` in `solve.py`); **not tested** against a quantity the capture carries (the captures carry no camera or light record). Evidence shown: distinct values per frame, and a file-parameter hint where one equals it (a SCENE constant that is really FILE would show there; RimLightParam3 did and is now mapped, FILE) |')
    w('| **RUNTIME** | anything else: game logic writes it | differs from the file in at least one joined draw, or no file parameter exists for it, or **partial**: the components the file names agree but a component with no file source takes several values. Split in the lists below into **varies** (more than one value in the corpus) and **one value** (a single value in every captured draw: a constant default, or the corpus is too small to say) |')
    w('| **UNJOINED** | a file source is declared but no draw using the constant could be joined to its mesh | no evidence either way |')
    w('| **UNSEEN** | in the effect\'s description, in no captured draw | the corpus is six scenes; expected |')
    w('')
    w('## Method (fixed; the join and its limits)')
    w('1. **Effect of a draw** = the effect(s) whose description lists the draw\'s vertex and pixel shader hashes (`hashes` in the `.fx.json`); the variant\'s '
      'constant list gives (field, register, count) per stage. The value is the vec4 run of that draw\'s constant bank (`vs_bank` / `ps_bank`).')
    w('2. **Join (draw -> mesh in the file).** Only draws the game itself named a model part (draw origin class `model_part`, NAMES.md) are joined; every other class has no '
      'mesh by design and is counted under "no file source". A model part carries `{object, part}` (the model object\'s guest address and the part index). '
      f"All parts of one object in the frame must fit one model of the atlas ({mesh_count} meshes): the part's (vertex count, index count) must equal the "
      'atlas mesh at that index, and the mesh\'s effect must be the draw\'s effect (LM variants: without the `LM`). The set of fitting meshes is the candidate set. '
      'Several models can fit (same counts, other attire): a constant is compared only when every candidate gives the same expected value for it; otherwise the draw gives no verdict '
      'for that constant (counted "ambiguous"). A draw whose file-mapped constants mostly disagree with the joined mesh (at least 3 compared, at least half unequal) is treated as '
      'a wrong join and counted as unjoined, not as evidence. **Those draws are not hidden:** each constant compared in them is counted as *disputed* on its own verdict line (whether the join was wrong or the game overrides the material is unknown) and listed below.')
    w('3. **File side.** `tools\\solver\\fieldmap.json` declares, per constant name, which parameter of the mesh (and which component) it should be copied from. The entries are hypotheses '
      '(from the loader and the S97/S98 value-match notes); a constant with no entry that has a parameter of the same name (`g_f4NAME`, `g_fNAME`, ...) in the effect\'s meshes is mapped by name. '
      'Components declared `null` are collected too: if one takes more than one value in the joined draws the constant is RUNTIME "partial". The capture decides: a hypothesis that the capture refutes shows as RUNTIME with its mismatch count, never as FILE. **A constant with no declared or same-name parameter is reported RUNTIME '
      'even if some other parameter is its real source** (the "value hints" below list file parameters that equal it in at least 90% of joined draws: the gaps to close).')
    w('   A FILE verdict covers **only the components that have a declared source** (a `null` entry in the field map is not judged: e.g. `Sweat` and `Blood` are FILE for the static '
      'components the file names, while the amount that moves with play is not in any component the map declares, so read a FILE row as "no hook needed for the file-named part").')
    w('4. **Planted faults** (gate lane `solver`): one FILE comparison perturbed in one draw must turn that (effect, constant) RUNTIME; a capture with its names stripped must report every draw unjoined.')
    w('')
    w('Limits, stated: the corpus is six zips (Bryan\'s entrance, 30 Years, the create editor, three Brock runs; the manifests do not record the resolution arm); the join is by geometry counts plus the object\'s part set, not by an '
      'asset path (the six captures carry no FILE texture provenance and no `asset` strings, see NAMES.md "Open"), so a model the files do not hold (the create editor\'s wrestler) is unjoined; a '
      'constant that only a different scene would change reads as "one value". Frames are sampled (evenly, first to last), not all read.')
    w('')
    w('## Checks that were made (gate lane `solver`, `py -I tools\\solver\\selftest.py`, ~50 s)')
    w('- **Positive controls** (`tools\\solver\\expect.json`): the constants S98 value-matched against a wrestler file (`asset_model.h` 55-60: SpecCubPrm1, Reflect, Param0, SSS, SSSSinCol2; the gear\'s '
      'noise set) come out FILE; the hair highlight colours S98 left "RUNTIME, unconfirmed" (`SCENE_FRONTEND.md`, SpeCol1 / SpeCol2) come out RUNTIME (about 5 of 6 joined draws differ from the file); '
      'bones and fade (WorldMatrixArray, MdlAlpha) RUNTIME; the camera SCENE.')
    w('- **Past fault** (acceptance 2): `RENDERER_ARC.md` 2.1 (the 49-row ledger) and the lines of `SESSION100_SUMMARY.md` that mention constants, banks or registers were read. None is a constant '
      'taken from the wrong source: the ledger\'s "effect descriptions" row is an `alpha_test` flag copied into 29 effects, and the bank/constant lines are speed work and the bank-sync marks. So there '
      'is nothing to re-find from S100; the planted faults below stand in, and the S98 controls above are the nearest real history.')
    w('- **Planted fault A**: one FILE comparison of `yCh_skin SpecCubPrm1` (pixel) and `yCh_skin MatAmbCol` (vertex, and pixel: a constant present in both stages) perturbed in one draw -> reported RUNTIME at the perturbed stage '
      '(a single deviant draw is enough, by design). Its unplanted twin is asserted FILE at the same frames per zip. Also asserted: no effect label holds two effects, disputed counts and partial verdicts exist, '
      '`pick_frames(n, 1)` works, and a run where nothing joined fails.')
    w('- **Planted fault B**: a capture with its names stripped -> every draw unjoined with the reason "unnamed", loudly (`names stripped: N draws, 0 joined`); a glob that matches no zip fails with `FAIL`.')
    w('')
    w('## Draws per scene')
    w(_scene_table(scenes))
    w('')
    w('### Unjoined, by reason (all scenes)')
    w('| reason | draws |')
    w('|---|---|')
    w(_reasons(scenes, 'unjoined'))
    w('')
    w('### No file source by design (all scenes)')
    w('| class | draws |')
    w('|---|---|')
    w(_reasons(scenes, 'no_file_source'))
    w('')
    w('## Per effect: constants by class')
    w('Only effects with at least one seen constant. `unseen` = in the description, in no captured draw.')
    w('')
    w('| effect | FILE | SCENE (declared) | RUNTIME | UNJOINED | unseen |')
    w('|---|---|---|---|---|---|')
    totals = collections.Counter()
    for eff, c in summ.items():
        if c['FILE'] or c['SCENE'] or c['RUNTIME'] or c['UNJOINED']:
            w(f"| {eff} | {c['FILE']} | {c['SCENE']} | {c['RUNTIME']} | {c['UNJOINED']} | {c['UNSEEN']} |")
            totals.update(c)
    w(f"| **all** | {totals['FILE']} | {totals['SCENE']} | {totals['RUNTIME']} | {totals['UNJOINED']} | {totals['UNSEEN']} |")
    w('')
    w('## The RUNTIME list = the hook list Phase C needs (effect -> constants)')
    w('`/v` vertex stage, `/f` pixel stage. **Varies** = more than one value across the captured draws (a game-side producer certainly exists). **One value** = the same value in every '
    'captured draw (may be a constant default needing no hook, or a producer this corpus never moved). Hints: a file parameter that equals the component in at least 90% of the joined draws '
    '(`k=param[i]`: component k), a lead for the field map, not a verdict.')
    w('')
    varies, one = collections.defaultdict(list), collections.defaultdict(list)
    for eff, recs in effects.items():
        for k, r in recs.items():
            if r['verdict'] != 'RUNTIME':
                continue
            nm = k.split(':', 1)[1] + '/' + r['stage'][0]
            if r.get('partial'):
                nm += ' [partial: ' + ','.join(r['partial']) + ']'
            if r.get('value_hints'):
                nm += ' [hint ' + ', '.join(r['value_hints'][:3]) + ']'
            (varies if r.get('varies') else one)[eff].append(nm)
    w('### Varies in the corpus')
    w('| effect | constants |')
    w('|---|---|')
    for eff in sorted(varies):
        w(f"| {eff} | {', '.join(sorted(varies[eff]))} |")
    w('')
    w('### One value in every captured draw')
    w('| effect | constants |')
    w('|---|---|')
    for eff in sorted(one):
        w(f"| {eff} | {', '.join(sorted(one[eff]))} |")
    w('')
    w('## FILE constants (the ones the model file settles, so no hook is needed)')
    w('`[unjudged x]` = a component with no file source that held one value in every joined draw (not proven FILE; a hook is needed only if another scene moves it).')
    w('| effect | constants |')
    w('|---|---|')
    for eff, recs in effects.items():
        f = []
        for k, r in recs.items():
            if r['verdict'] == 'FILE':
                nm = k.split(':', 1)[1] + '/' + r['stage'][0]
                u = r.get('unjudged', {}).get('one_value')
                f.append(nm + (f" [unjudged {','.join(u)}]" if u else ''))
        if f:
            w(f"| {eff} | {', '.join(sorted(f))} |")
    w('')
    w('## Disputed (draws whose join was rejected, counted on the constant)')
    w('| effect | constant | draws compared in rejected joins | of them equal to the file | verdict |')
    w('|---|---|---|---|---|')
    for eff, recs in effects.items():
        for k, r in recs.items():
            if r.get('disputed'):
                w(f"| {eff} | {k.split(':', 1)[1]}/{r['stage'][0]} | {r['disputed']['draws']} | {r['disputed']['agreeing_with_file']} | {r['verdict']} |")
    w('')
    w('## SCENE constants (declared by name, untested; camera / lights / time: the scene record carries them)')
    w('Brackets: median distinct values per frame. `[hint ...]`: a file parameter equals this component in >= 90% of joined draws (mostly coincidences of a default; check before trusting the SCENE label).')
    w('| effect | constants |')
    w('|---|---|')
    for eff, recs in effects.items():
        f = sorted({k.split(':', 1)[1] + '/' + r['stage'][0] + (f" ({r['values_per_frame']['median']})" if r.get('values_per_frame') else '')
                    + (' [hint ' + ', '.join(r['value_hints'][:2]) + ']' if r.get('value_hints') else '')
                    for k, r in recs.items() if r['verdict'] == 'SCENE'})
        if f:
            w(f"| {eff} | {', '.join(f)} |")
    w('')
    w('## UNJOINED')
    for eff, recs in effects.items():
        f = sorted(k.split(':', 1)[1] + '/' + r['stage'][0] for k, r in recs.items() if r['verdict'] == 'UNJOINED')
        if f:
            w(f"- {eff}: {', '.join(f)}")
    w('')
    return '\n'.join(out) + '\n'
