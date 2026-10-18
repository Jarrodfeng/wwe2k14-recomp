# tools/db

Checks for the function database in `db\*.json` (Session 8, task 18). Reads only our own db and docs files, plus
the gitignored code index (`out\harness\index\index.db`, built by `tools\q\build_index.py`) or, without it, the
generated function list, for the address checks. Prints addresses and our own names only.

    py tools\db\validate.py                  text report: errors, warnings, label table, coverage tables
    py tools\db\validate.py --warn-layout    non-address entries (struct layouts, offsets, ranges) become warnings
    py tools\db\validate.py --json           machine-readable (errors, warnings, address_forms, key_labels, coverage)
    py tools\db\validate.py --no-index       use generated\default\*_init.cpp instead of the q index
    py tools\db\validate.py --list 200       more lines per error/warning kind

Exit code: 0 no errors, 1 errors, 2 no db folder, 3 internal error of the validator (a traceback, not a verdict).

What it checks (full list in the docstring of `validate.py`):

- Schema (CLAUDE.md): every field present, confidence and system in their enums, evidence a list of strings and
  not empty above `low`.
- Addresses: one 8-digit hex guest address per entry (others are listed by kind: no address, address plus offset,
  several addresses or a range; `q db` cannot find them); code-range addresses must be function starts.
- Duplicates: within a file, and the same address under different names or confidence across files (review list).
- Hard rule: text that looks like generated C++, and entries quoting 3 or more PPC instructions (`disassembly`).
- Key systems (camera, match state, video/tron, cutscene/motion, file loading): who labelled each entry, read from
  the db's own `labels: ...` evidence item. Per file: two labels / partial (the second labeller only mentions it) /
  single / none / unparsed. CLAUDE.md asks for two independent labels on these. The parse is free text: a lead.
- Coverage per db file and per system: confidence mix, % with live evidence (keyword match; negated mentions such
  as "not traced" or "never ran in coverage" do not count), % live only through coverage, % of function entries
  seen in the scenario coverage (`out\harness\coverage\coverage.json`).
