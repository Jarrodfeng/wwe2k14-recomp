# FAKE: an invented probe note for tools/builder/pack.py --selftest (our own text; cites only the fixture tree)

## Answer

The widget overflows because `src/alpha/widget.cpp:20-22` never clears the level before a reset (forward slashes, a range).
The big sum at `src\alpha\big.cpp:40` reads past the end (a non-fix site in a 66-line function: the size-cap case).
The game side is in listing 77.cpp:10 and `generated\default\recomp.12.cpp:2`, entry `sub_82005678` (store at 0x82001234).
Helper `fake_tool.py:11` counts the bumps (a bare name found only under tools\).

## Fix sites

1. `src\alpha\widget.cpp:12` give the widget a clamp before the add (backslashes, one line).
2. Three bare-name sites: `widget.cpp:12,32,41-42` share one rule (a list; 12 merges with site 1).
3. `core.h:5` is ambiguous on purpose: two headers share the name.
4. `src\beta\core.h:5` add a field to `BetaRow`; the layout constant moves and `:8` the layout word changes too.
5. `src/alpha/mover.cpp:10` the step add (it moved down since the note was written).
6. `src/alpha/mover.cpp:16-17` in `TickMover` (rewritten since the note was written).
7. `src\alpha\widget.cpp` `ResetWidget` needs a reset flag (a file with a function named beside it, no line).
8. `widget.cpp:27` the lambda inside `Helper` (the block is the enclosing function, not the lambda).
