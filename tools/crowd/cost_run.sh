#!/bin/bash
# usage: run.sh TAG  -> out/l270/TAG_{brock,match,checkb}.{log,hash}
cd /c/Users/Jarrod/wwe2k14-recomp/.claude/worktrees/agent-a15f35daef8dba948
G=/c/Users/Jarrod/wwe2k14-recomp/game_data/extracted
export RDR_CROWD_PHASES=${PH:-1} RDR_CROWD_VERIFY_EVERY=${VE:-30}
tag=$1; shift
for c in brock:out/l270/brock/capture:300 match:out/l270/match/capture:300 checkb:/c/Users/Jarrod/wwe2k14-recomp/out/s101/crowd/case_brock/capture:400; do
  IFS=: read n cap fr <<< "$c"
  out/renderer/release/fview.exe --capture $cap --crowd-ours --crowd-cards --game $G --frames $fr --replay --hash-frames out/l270/${tag}_$n.hash "$@" > out/l270/${tag}_$n.log 2>&1
  echo "$n rc=$? $(grep -c . out/l270/${tag}_$n.hash) hashes"
done
