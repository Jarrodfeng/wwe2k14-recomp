# Parked SDK patches

Diffs of SDK work that is **not** on `wwe2k14-local`. They are not part of the verified chain in `..\full\` (`export_series.py`
ignores this folder), and applying them is a decision for the main session.

| File | What | Why parked |
|---|---|---|
| `104-session99-scaled-readback-rumble.diff` | Session 99 (#173): the scaled scene pass stays scaled inside read-back windows, read-back copies written from the blitted-up image; (#180, unconfirmed): unchanged rumble values are not re-sent to SDL while the armed effect has time left. SDK branch `s99-scaled-readback-rumble` (323585f, on top of 153678b). | Session 99 kept it, not merged. It is not an ancestor of `wwe2k14-local`. Number 104 is reserved in `export_series.py` (`RESERVED`) so patches 105-108 keep their numbers. If it is ever merged, take it out of `RESERVED`, add its STEPS line and move the file back to `..\full\`. |

Source: `git log --all -- patches/rexglue-sdk/full/104-session99-scaled-readback-rumble.diff` (commit 167627f8, Session 99).
