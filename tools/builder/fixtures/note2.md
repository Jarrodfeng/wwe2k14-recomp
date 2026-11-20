# FAKE: a second invented note for tools/builder/pack.py --selftest (review findings and round 2 cases; cites only the fixture tree)

## Fix

1. `zip.cpp:10,50` two lines in one list.
2. `zip.cpp:20-22` one range.
3. `renamed.cpp:3` the return line.
4. `src\alpha\widget.cpp:400` a line past the end.
5. `shapes.cpp:4` the C function, `:13` the pick, `:17` the after, `:22` the constructor body, `:34` the second hook.
6. `unbalanced.cpp:7` the later function.
7. `doc_tool.py:8` the line after the string.
8. `src\alpha\limits.h:3` `kWidgetMax` moves from 10 to 12; its readers change too.
9. `hidden_only.h:1` only in an untracked folder; `out\scratch\thing.cpp:2` is scratch.
