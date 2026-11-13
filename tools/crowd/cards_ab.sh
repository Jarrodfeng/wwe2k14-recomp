#!/bin/bash
# lane 219: A (--crowd-ours) vs B (+ --crowd-cards) presented-surface hashes of one case zip: how many frames differ, and the cards census of B.
#   GAME=<game_data/extracted of the main checkout> [FRAMES=4000] [FVIEW=path/to/fview.exe] bash tools/crowd/cards_ab.sh ZIP [extra fview args]
# Output (hash CSVs, logs) goes to out/cards_ab/ of this checkout; only the unpacks this script made are removed.
HERE=$(cd "$(dirname "$0")/../.." && pwd)
[ -n "$GAME" ] && [ -d "$GAME" ] || { echo "set GAME to the game_data/extracted folder" >&2; exit 2; }
FVIEW=${FVIEW:-$HERE/out/renderer/release/fview.exe}
Z=$1; shift
W=$HERE/out/cards_ab; mkdir -p "$W"
for arm in A B; do
  X=""; [ $arm = B ] && X="--crowd-cards"
  before=$(ls -d "$HERE"/out/cases/fview_unpack_* 2>/dev/null)
  "$FVIEW" --case "$Z" --replay --frames ${FRAMES:-4000} --validation 1 --crowd-ours $X --game "$GAME" --hash-frames "$W/$arm.csv" "$@" > "$W/$arm.txt" 2>&1
  for d in $(ls -d "$HERE"/out/cases/fview_unpack_* 2>/dev/null); do
    echo "$before" | grep -qxF "$d" || rm -rf "$d"
  done
done
paste -d' ' "$W/A.csv" "$W/B.csv" | awk -F'[, ]' '$1!="format" && $2!=$4{n++; if(n==1)first=$1} END{print n+0" of "NR-1" frames differ, first "first}'
grep -a -o "cards: frames asked.*" "$W/B.txt" | head -1 | sed 's/; record cards.*types with the record/ ... types with the record/' | cut -c1-700
