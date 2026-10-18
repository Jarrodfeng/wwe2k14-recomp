# db/ids: id tables (our annotations)

Lookup tables for ids the game uses, kept apart from the function database: `tools/db/validate.py` and `tools/q`
read only `db/*.json` (lists of function entries), not this folder.

| File | What | Sources |
|---|---|---|
| `characters.json` | character ids (wrestlers, announcer, referee, commentators, valets): the id is also `pac\ch\CHnnn.PAC` and the titantron `movies\titantron\NNN.bik2`; team titantrons separately | Session 9 captures and runs (`PLAYBOOK.md` Session 9), Jarrod's screenshots and on-screen observations, and the community list cited in the file |
| `arenas.json` | arena ids (the arena setter's value = `pac\bg\BGnn.PAC`) and the arena select grid layout | Session 9 captures and runs, Session 8's icon-table reading (`docs/systems/match_setup.md`), Jarrod's grid screenshot |

Rules: every entry lists its evidence and a confidence; only ids we have seen ourselves are listed (a community
source is cited, not copied); names are public ring / event names; no game data. Unknown stays unknown.
