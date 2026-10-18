#!/bin/bash
# usage: run.sh SCRIPT.txt "EXTRA FLAGS" -> prints the run line, then the run's own log path (from the run number)
sc="$1"; ex="$2"
p="C:/Users/Jarrod/wwe2k14-recomp/tools/input_scripts/short/$sc"
line=$(pwsh -NoProfile -Command "& 'C:/Users/Jarrod/wwe2k14-recomp/tools/bench/run_list.ps1' -Scripts @('$p') -Extra '$ex'" 2>&1 | tail -1)
echo "$line"
n=$(echo "$line" | grep -o "wwe2k14_[0-9]*" | head -1 | sed 's/wwe2k14_//')
echo "/c/Users/Jarrod/wwe2k14-recomp/out/build/win-amd64-release/logs/wwe2k14_${n}.log"
