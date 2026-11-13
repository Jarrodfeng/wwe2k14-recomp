# The renderer arc: review of the 20:27 progress report

Written 2026-11-11 20:3x by a review session (Fable 5.2) at Jarrod's request, for the main session (Opus 5.5).
**Read:** `RENDERER_ARC.md` (cited as **A**) and `RENDERER_ARC_PROGRESS.md` (cited as **P**), and nothing else. I had no access to the repo.
Every fact about the repo below is quoted from those two files; where a suggestion depends on a fact I could not check, it is marked **[U]**.
Nothing here is a decision. Section 5 lists what needs Jarrod's word.

---

## 1. Verdict

The plan holds. The execution model (lanes, contracts first, offline proof, the gate) is working, and the Opus review is a real addition.

The weakness is one thing seen from two sides: **almost everything since Check A is proven against our own viewer and has never met the live game.** The pile of compile-only code is growing, and the offline reference it is judged against is known to differ from the game.

---

## 2. What I would change, in order

### 2.1 Go live sooner, in smaller steps
**Why.**
- A 5A measured the game running at 5% of Session 100's clock (median run 29 s). Runs felt brutal because each was a full orchestrator turn.
- A 2.1: the dear faults were the ones only a live game had (timing, who wrote memory). Offline proof cannot find that family.
- P 5.1 lists what is compile-checked only: the provenance fixes, UI live names, the complete zip, the scene writer, three class contributors, moments.
- Decision 1 names what makes a run brutal: the waiting, the PC tied up, no answer. A hidden, low-priority run under the run contract with automatic intake has none of the three.

So "0 game runs since Check A" is integration debt, not thrift.

**Do.** Split Check B into three, each attributable to one kind of change:

| Step | Build state | Passes when |
|---|---|---|
| B-a | everything merged, every new switch **off** | the five owner scenes match Check A (owned_check + frame times): off is really off |
| B-b | names, provenance fixes, UI live names, the complete zip | unnamed count 0 or listed; intake reads every zip; provenance agrees with the frame viewer on spot checks |
| B-c | scene records **on, record-only**, all classes, judge-mode captures | records for every class in the capture; the on / off hash streams are equal on one recording (2.7) |

Nothing is drawn from a scene record live in Check B. All judging of B-c's captures happens offline afterwards.

Small probes that can ride B-c or take one short run each: the bank-id probe (2.6), the Create a Superstar trace for N2, the action-id log for N4, and one boot of the 46-open test save (P: "the executor has never run").

**Then make live contact a lane, not a milestone.** After each merge batch: one hidden run under the run contract through a fixed short address list (title, one exhibition match, an entrance, the bell), black box saved, intake automatic, the result on the board. The orchestrator reads it only when it fails. This needs Jarrod's word because it changes how rule 7's budget is counted (section 5).

### 2.2 Fix the reference
**Why.**
- P 2, Phase B last row: rdr matches the viewer nearly everywhere, and **both are flatter and brighter than the game** (live light set and post chain missing).
- B2's proof and the C2-C4 contributor proofs are all "against the viewer's picture".

So "wrestlers 235 / 235 pass" means rdr agrees with the viewer and its detectors. It does not yet mean "looks right" (decision 5), and no sheet against the game can get a verdict from Jarrod while a known global difference covers every pixel.

**Do.**
1. **Post chain: a lane now** (see 3, question 4).
2. Light match (lane 33) continues: it supplies the game's camera and evaluated lights.
3. Once both are in, the reference for a file-fed picture becomes a game capture at the same camera and light, through the judge.
4. Until then, every census and proof line **names its reference**: "matches viewer", not "pass".

### 2.3 Fresh orchestrator before Check B, not after
- A 5A rule 8 asks for a fresh context per phase. P 5.5: this one is ~60% full and has run through Phase B's end and Phase C's start.
- Debugging the first live run of this much code in that context is Session 100's shape again. Hand off first.
- A 5A rule 10 asks for one STATUS line per round from `session_time.py` (lanes active, foreground compile, game runs). P 3 gives agent counts and usage instead. Agent counts do not show whether the orchestrator has become the line again: ~50 agents and 14 reviews in 74 minutes all merge through one context "in file order". Please report the same table as A 5A for 16:2x-20:27.
- If that table shows the merge queue is where time goes, consider an integrator lane that merges reviewed hand-backs and runs the gate, leaving the main session conflicts only.

### 2.4 Jarrod's button session now, on PC
- It is the third part of Check A and the thing decision 7 calls key. P 2: PC is ready; it waits on a phone APK it does not need.
- Phase D's six open faults (title-logo glitch, carry / pin darkening, the Undertaker's blocks, 120 on a 60 Hz panel, crowd flashes, Bryan's entrance) each need a case (A 9, rule 2). P shows none closed and none cased.
- Start the coverage board now. It is a table, A says the gate can be reached before C completes, and it tells every lane which work the release actually waits on.
- Toggle vs always-on: the default taken ("on in his test APKs") is fine; do not block on it.

### 2.5 Phone and Surface
- P 4.6: no work today. Decision's goal: flawless on both Android and PC.
- A already puts the phone in the offline loop: B2's proof ends "then the phone", tool 5's census runs "on the PC and the phone", tool 7's farm includes the Surface and the phone.
- The farm finding in P (a CPU-bound PC hides GPU regressions) is a second reason: the phone and Surface are where the GPU is the limit.

**Do.** The rdr census on the phone, and the corpus replay on phone and Surface. Both are files-only or capture-only. **[U]** whether the phone's viewer build can carry the rdr backend without the game-side merges; if it can, this does not need to wait for them.

### 2.6 Confirm the bank-id finding live before building further on it
- P N3 is marked "[C listings, U live]", and P 4.1 says the first reading concluded the opposite.
- One probe per path with one known id: the cutscene startup sequence from the menu, `sub_82738060` mid-match, `sub_827C4430` for a victory.
- Lane 32 is already building three address forms on it. A wrong assumption found at its check costs a lane; found now it costs a run.

### 2.7 Hash checks on every hook, since the tool exists
N5 gave a hash-stream track check. Use it beyond the scene director:
- every read-only contributor: records on vs off on the same recording, streams equal. "Only reads" becomes a checked fact.
- the bar for hooks that change the game is in section 4, risk 2.

### 2.8 Self-tests and the review
- P 3: several builder self-tests were circular or could only find their own plant (the multiply-order test, the census "half arena" detector, the widget name bug passing a Python-written frame).
- A planted fault written by the builder tests the builder's idea of the fault. **Have the reviewer write the plant**, blind to the detector's code, and keep preferring a real past bug re-found.
- Feed each review's bug class back into `common.md`, as already done for three. Track the review hit rate; it is 14 of 14 today, which says the builders' acceptance tests are still too weak.

### 2.9 Smaller items
- **The judge's cause rule** picked effects by draw count: an inferred rule. The frame viewer already lists the draws that wrote a pixel, with names. Use that list.
- **"Props have no per-frame list (take them from the draw)"** is inference from the stream. Fine as a P1 step, but it needs the comment A 9 rule 1 requires, and a line in section 8's unknowns.
- **"Phase B is nearly done"** with arenas at 24 pass / 14 tile fault / 19 no effect yet: 33 of 57 not passing. Say that in the one paragraph.
- **The solver's SCENE class is "declared, untested".** Lane 33's captured camera and lights are the test.
- **Check A went one run over.** One line in the summary on why, so the run contract can learn from it.
- **No P3 work exists yet.** Not drift at four hours, but name the first one so "we drive" is not deferred forever. A 3.4 already picks it: UI at the display's own resolution.

---

## 3. Answers to P section 6

**1. Order of the live checks.** Separate recording from promotion. Record all classes together in one record-only step (B-c): they only read, and one set of captures feeds every class's judge offline. Promote one class per check, UI first, in the arc's order: a promotion skips the game's own render work for the class, so a failure must point at one switch. Building C2-C4 in parallel offline changes nothing in the ladder.

**2. The scene director and 4A.** It simplifies the design; the design's shape holds.
- `scene:ID` becomes the canonical address. `wm30:N@moment:K`, `cutscene:ID`, `@victory` and `@entrance:N` become names that resolve to ids through one table.
- That table is a **scene atlas**, built from the pac index's `/EVP/<id>` entries with 0 runs: id, family (by range), name, and what the scene needs around it (a match, participants, an arena).
- One id space is a closed world, as the drawables atlas is: a **scene census** becomes possible, every id played unattended with a case each. Entrances and victory scenes are in the release gate, so this is Phase D coverage too.
- An id alone is not an address; it needs its stage: `match:SPEC@scene:ID`, as lane 32 has it.
- Resolver order is unchanged: builders first. The scene manager is one builder that covers what A listed as four.
- Unchanged: the widget driver, the situation director (a pin or a carry is not a scene), moments from recordings, and the unlocked save (a player-like walk still needs it).
- Add one flag: an address reached by forcing a scene says so ("forced") in the frame and the case zip. A 30 Years moment played by id skips whatever the objective script set up before it.

**3. An Opus review per hand-back as a rule.** Yes. 100-200k tokens is small beside a fault that reaches a live check. Scope it so it stays cheap later: always for anything touching a hook in the game exe, one of the three formats, or a check tool (a wrong check tool is the ledger's "tooling" family: 9 rows, ~20 runs); for the rest, every hand-back while the hit rate stays high, sampled once it falls. Add the reviewer-written plant (2.8).

**4. The post chain's place.** Its own lane, now, ahead of the live checks of C1-C4.
- It is what makes "does it look right" judgeable against the game (2.2).
- It can be proven with **0 runs**: take a capture's scene colour and depth just before the post chain, run our post pass on them with the constants the final pass carried, and compare with that same capture's final picture. Input and expected output both come from the game.
- It is independent of every other class: its inputs are surfaces with roles and constants with names.

**5. Drift from the arc's intent.**
1. The viewer standing in as the reference (2.2). The arc says "renders the game as intended", not "as our viewer does".
2. Offline proof becoming the goal instead of the first gate (2.1).
3. Props from the draw (2.9).
4. The phone absent from a day's work (2.5).

Not drift: light match and live shader reload (neither is a feature under the freeze), Phase C in parallel, and the second scene reading.

---

## 4. On P section 5's risks

**Risk 1 (Check B is a big first step).** Split it: 2.1.

**Risk 2 (hooks inside game logic).** A hash stream is the right tool; the bar differs by state:
- **Off:** the hash stream is identical to the baseline on a recording. A default-off hook is inert.
- **On:** (a) repeatable: the same address reached twice from the same start gives the same stream; (b) the game is left valid: the match runs to a normal end and returns to the menu, every wait on an event; (c) frames after a forced start carry the "forced" flag, and a fault seen only in a forced state is reproduced once by a natural route (a moment recording) before it is fixed as a game fault.
- Out of the player build until Jarrod says the sandbox ships (my judgement).

**Risk 3 (space convention).** Agreed, and before any live record is captured: records in two spaces would poison the corpus. Make it a declared field in the record's header (space, handedness, units) under A 9 rule 1, bump the format version, and add one gate test: a known object placed through each class lane lands in the same place.

**Risk 4 (the judge is slow).** The snapshot fix is right. Until it lands, do not plan B-c's offline judging as a corpus sweep.

**Risk 5 (orchestrator context).** 2.3.

**Risk 6 (post chain).** Question 4.

---

## 5. Proposed changes to the arc (on Jarrod's word)

1. **Live smoke lane.** After each merge batch, one hidden contract run on a fixed address list, counted apart from check runs in rule 7's budget. No merged work waits more than one batch to meet the game.
2. **A pass names its reference.** No tool reports "pass" without saying against what: the game (a capture), the viewer, or its own detector.
3. **Review per hand-back,** scoped as in question 3, with reviewer-written plants.
4. **`scene:ID` is the canonical scene address;** the scene atlas and scene census join section 4's tools.
5. **The post chain is its own class,** ahead of C5's remainder.
6. **Every hook gets an on / off hash check;** hooks that change the game get the "on" bar in section 4.
7. **Check B is three steps** (2.1), record-only; promotions follow one class at a time.

---

## 6. What I could not check

Everything. No file in the repo was opened, no number re-derived, no picture seen. In particular: what the census's "pass" compares against beyond what P states, whether the phone build can take rdr alone, and whether the pre-post surfaces in today's captures are complete enough for the post-chain proof in question 4. Treat each as a lead.
