// S111 M100-COMP (MENUS100 wave 2): the menu questions the equivalence tester answers, one header each, registered here. Included by
// equiv_tests.cpp inside namespace equiv, after the Test / Field types and the placement namespace; AddMenus100 is called by Tests().
// (Our own headers come in through menus100_pre.h at the top of equiv_tests.cpp: nothing is #included from here.)
#include "menu_lookup_test.h"     // Q5.3: sub_83424808 vs src\menu_goto_core.h
#include "resolver_gate_test.h"   // Q3.5: sub_8283A900 (the resolver's code-1497 test) vs src\launch_gate.h ResolverGate
#include "text_bank_test.h"       // Q11.1: GetString sub_822563F8, id -> key slot, vs the note's rule

void AddMenus100(std::vector<Test>& v) {
  Test a;
  a.name = "menu_lookup", a.target = 0x83424808, a.fields = lookup::Fields();
  a.what = "M100-COMP Q5.3: the flow table (menu, item) lookup vs src\\menu_goto_core.h ReadTableWith / MarkSeen (ours called directly)";
  a.game = lookup::Game, a.ours = lookup::Ours;
  v.push_back(a);
  Test a2 = a;
  a2.name = "menu_lookup.twin_noindex", a2.what = "planted twin: the lookup ignores the menu's index range", a2.ours = lookup::OursTwinNoIndex, a2.twin_of = "menu_lookup";
  v.push_back(a2);
  Test b;
  b.name = "resolver_1497", b.target = 0x8283A900, b.fields = resolvergate::Fields();
  b.what = "M100-COMP Q3.5: the resolver's code-1497 test (SM+364 <= 100 signed) vs src\\launch_gate.h ResolverGate (ours called directly)";
  b.game = resolvergate::Game, b.ours = resolvergate::Ours;
  v.push_back(b);
  Test b2 = b;
  b2.name = "resolver_1497.twin_unsigned", b2.what = "planted twin: the menu compared unsigned", b2.ours = resolvergate::OursTwinUnsigned, b2.twin_of = "resolver_1497";
  v.push_back(b2);
  Test c;
  c.name = "text_bank", c.target = 0x822563F8, c.fields = textbank::Fields(), c.stubs = textbank::Stubs();
  c.what = "M100-COMP Q11.1: GetString's id -> key slot (40000..79999 by 60000 / 50000 / 45000) vs the MENUS100_INV note's rule (a model; list lookup stubbed)";
  c.game = textbank::Game, c.ours = textbank::Ours;
  v.push_back(c);
  Test c2 = c;
  c2.name = "text_bank.twin_swap", c2.what = "planted twin: the 50000 and 45000 slots swapped", c2.ours = textbank::OursTwinSwap, c2.twin_of = "text_bank";
  v.push_back(c2);
}
