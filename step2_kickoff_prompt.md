Session 45, unattended (I'm at work for about 8 hours): step 2, the native frame renderer, the way Unleashed Recompiled
does it. I want you to really lock in and see how far you can take it.

Start with `py tools\harness\session_start.py`, then read `SESSION45_PLAN.md` (sections 0-2b) and
`docs\native\STEP2_DESIGN.md`. Follow the plan in order: N0 baseline, N1 every draw of a match frame native, N2 no
EDRAM, N3 no memory round trip, measuring after each (N4), and N5 groundwork only if N1-N3 are done or parked.

The goal is the memory picture: our targets become the truth, the EDRAM and the trip through guest memory go away,
like Unleashed's renderer, so integrated graphics can get to 30 fps. This one is allowed to not be bit-exact, but:
- the game's behaviour must not change (determinism identical after every milestone);
- every picture change is measured with the parity harness (`s45_parity_shots` + `parity_pair.py`) and reported in
  numbers; I'll judge the pictures by eye when I'm back, so keep the difference pictures;
- anything that can still be exact is checked exactly.

Rules for today:
- Never stop to ask me anything. Decisions that are mine go into `SESSION45_SUMMARY.md`'s "For Jarrod" list, and you
  take the safe default.
- Native branches only. No merge, push, test build, release or new defaults: everything new stays behind settings that
  are off by default, LOW only.
- No timed runs except the full regress in the wrap-up. Never touch my save or my launcher settings.
- Commit, pin and add a line to the plan's progress log (section 7) after every sub-step, and keep
  `SESSION45_SUMMARY.md` and the PLAYBOOK current as you go. Take times from Get-Date or the run logs, never guess.
- Timebox each sub-step to about 2 hours or 3 failed attempts; write down what you found and move on.
- The reviewer agent once per milestone on the SDK changes, the shot-checker for screenshot batches. No big fan-outs.

If you lose your place (e.g. after an auto-compaction), re-read the plan's sections 0-1 and the progress log before
doing anything else. Finish with the plan's wrap-up (section 4) and write `SESSION46_PLAN.md`.
