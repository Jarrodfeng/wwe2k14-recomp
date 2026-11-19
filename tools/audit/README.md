# Audit scripts (S103 review session, moved into the repo 2026-11-17 at Jarrod's wish: 'the auditing is how we stay efficient, we should keep to it')

They print numbers only, from the session transcripts and out\gate\times.csv (nothing from the game).

- agent_cost.py DIR [SINCE_UTC]
- agent_split.py DIR SINCE_UTC
- agent_long.py DIR SINCE_UTC name1,name2 [TOP]
- main_cost.py FILE LO_UTC HI_UTC
- gate_times.py times.csv ["YYYY-MM-DD HH:MM:SS" local]

DIR = %USERPROFILE%\.claude\projects\C--Users-Jarrod-wwe2k14-recomp\<session id>. Unit weights: input 1, cache write 1.25, cache read 0.1, output 5. tools\labelling\agent_cost.py is an older, different tool.
