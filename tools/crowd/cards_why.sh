#!/bin/bash
# lane 219: the cards census (incl. the fallback reasons) of fview --crowd-ours --crowd-cards on each case zip given.
#   GAME=<game_data/extracted of the main checkout> [FRAMES=4000] [EXTRA="fview args"] [FVIEW=path/to/fview.exe] bash tools/crowd/cards_why.sh ZIP...
# fview = out/renderer/release/fview.exe of this checkout unless FVIEW is set. Logs go to out/cards_why/ of this checkout. Each unpack fview makes
# (out/cases/fview_unpack_*) is removed after its zip, and only the ones this script created (the directory listing before / after).
HERE=$(cd "$(dirname "$0")/../.." && pwd)
[ -n "$GAME" ] && [ -d "$GAME" ] || { echo "set GAME to the game_data/extracted folder" >&2; exit 2; }
FVIEW=${FVIEW:-$HERE/out/renderer/release/fview.exe}
W=$HERE/out/cards_why; mkdir -p "$W"
for Z in "$@"; do
  echo "### $Z"
  before=$(ls -d "$HERE"/out/cases/fview_unpack_* 2>/dev/null)
  "$FVIEW" --case "$Z" --replay --frames ${FRAMES:-4000} --validation 1 --crowd-ours --crowd-cards --game "$GAME" $EXTRA > "$W/last.txt" 2>&1
  echo "rc=$?"
  grep -a -o "validation errors [0-9]*" "$W/last.txt" | tail -1
  grep -a -o "cards: frames asked.*" "$W/last.txt" | head -1 | sed 's/; record cards.*types with the record/ ... types with the record/'
  grep -a "WARN cards" "$W/last.txt" | head -3 | cut -c1-300
  for d in $(ls -d "$HERE"/out/cases/fview_unpack_* 2>/dev/null); do
    echo "$before" | grep -qxF "$d" || rm -rf "$d"
  done
done
