#!/bin/bash
# usage: bench.sh N  -> alternates base / new N times; logs in out/l270/bench/
cd /c/Users/Jarrod/wwe2k14-recomp/.claude/worktrees/agent-a15f35daef8dba948
G=/c/Users/Jarrod/wwe2k14-recomp/game_data/extracted
export RDR_CROWD_PHASES=1 RDR_CROWD_VERIFY_EVERY=30
mkdir -p out/l270/bench
BASE=/c/Users/Jarrod/wwe2k14-recomp/.claude/worktrees/l270base/out/renderer/release/fview.exe
NEW=out/renderer/release/fview.exe
for i in $(seq 1 $1); do
 for tag in base new; do
  exe=$NEW; [ $tag = base ] && exe=$BASE
  for c in brock:out/l270/brock/capture:300 match:out/l270/match/capture:300 checkb:/c/Users/Jarrod/wwe2k14-recomp/out/s101/crowd/case_brock/capture:400; do
    IFS=: read n cap fr <<< "$c"
    $exe --capture $cap --crowd-ours --crowd-cards --game $G --frames $fr --replay > out/l270/bench/${tag}_${i}_$n.log 2>&1
  done
 done
done
echo done
