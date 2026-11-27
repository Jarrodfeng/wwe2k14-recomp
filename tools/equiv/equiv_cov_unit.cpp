// S111 EQUIV (review): the coverage tier's entry decision (src\equiv\equiv_cov.h OnEntry) on its own, no exe, no game. tools\equiv\equiv.py
// writes the allowlist WITHOUT the callees a test stubs (a stubbed function's body never runs), so a stub must answer before the allowlist
// is asked; a stub that lets the body run (returns 0) still needs that body allowlisted. Built by tools\equiv\build_equiv.ps1 -Standalone, run by
// tools\equiv\equiv_selftest.py (check cov.entry). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include <cstdio>

#include "equiv/equiv_cov.h"

namespace {
int g_calls = 0;
int Skip(const Wwe2k14EquivHost*) { return ++g_calls, 1; }
int RunBody(const Wwe2k14EquivHost*) { return ++g_calls, 0; }
}  // namespace

int main() {
  using namespace wwe2k14::equiv;
  int bad = 0;
  auto check = [&](const char* name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    bad += !ok;
  };
  CovState& s = Cov();
  s.allow = {0x82000010u};
  s.stubs[0x82000020u] = Skip;     // stubbed and (as equiv.py writes it) not in the allowlist
  s.stubs[0x82000030u] = RunBody;  // a stub that lets the game's body run after it
  check("allowlisted runs", OnEntry(0x82000010u) == Entry::kRun);
  check("a helper (no address) runs", OnEntry(0) == Entry::kRun);
  check("outside the allowlist stops", OnEntry(0x82000040u) == Entry::kOutside && s.outside.load() == 0x82000040u);
  check("a stubbed callee outside the allowlist is answered by its stub", OnEntry(0x82000020u) == Entry::kStubbed && g_calls == 1);
  check("a stub that runs the body needs the body allowlisted", OnEntry(0x82000030u) == Entry::kOutside);
  s.allow.insert(0x82000030u);
  const int before = g_calls;
  check("... and runs it once allowlisted", OnEntry(0x82000030u) == Entry::kStubRan && g_calls == before + 1);
  std::printf("equiv_cov_unit: %d failed\n", bad);
  return bad ? 1 : 0;
}
