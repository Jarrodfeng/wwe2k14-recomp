// S111 M100-COMP, Q3.5 (docs\research\s111\MENUS100_INV.md): the part of the SmartMenu's resolver sub_8283F7E8 that OUR code mirrors, called
// directly. Included by equiv_tests.cpp (menus100_tests.h) inside namespace equiv; not a header of its own.
//
// What our shipped code (src\launch_gate.h, src\launch_core.h) mirrors of the resolver is ONE test: ResolverGate refuses a record with code 1497
// when the menu on screen is above 100 (signed), the test of sub_8283A900 (+10..+1C: SM+364 <= 100 signed). Every other test of the resolver
// is the game's own: launch_core.h Gates asks sub_8283F7E8 itself for each record on a path (GateRec::resolves), so none of them is ported.
// The target is therefore sub_8283A900 (the resolver's callee for code 1497); ours = mode_launch::ResolverGate on a GateRec{code 1497,
// menu = SM+364, resolves = true} (the game's own other answers already folded into `resolves`): "enters" = no refusal text.
// Held at their launch values, not generated: SM+11492 = 0, SM+11500 = 0, [SM+11412] = null (the three other tests of sub_8283A900: the
// resolver's answer already carries them, ours does not model them).
namespace resolvergate {

std::vector<Field> Fields() { return {IntField("[in_r3+364]", -3, 250, 1, {-1, 0, 99, 100, 101})}; }

int Game(const Test& t, const Case& c, Out& out, std::string&) {
  const uint32_t sm = H->scratch(11600, 16);
  std::memset(G(sm), 0, 11600);
  W32(sm + 364, uint32_t(int32_t(t.I(c, "[in_r3+364]"))));
  H->set_gpr(3, sm);
  const int rc = H->call(t.target);
  if (rc != 1) return rc;
  out["enters"] = (uint32_t(H->get_gpr(3)) & 0xFF) != 0 ? "yes" : "no";
  return 1;
}

void Ours(const Test& t, const Case& c, Out& out) {
  wwe2k14::mode_launch::GateRec r;
  r.code = wwe2k14::mode_launch::kCodeMenuGate, r.menu = uint32_t(int32_t(t.I(c, "[in_r3+364]"))), r.resolves = true;
  out["enters"] = wwe2k14::mode_launch::ResolverGate(r).empty() ? "yes" : "no";
}

// Planted twin: the menu compared unsigned (a negative menu is then "above 100").
void OursTwinUnsigned(const Test& t, const Case& c, Out& out) {
  out["enters"] = uint32_t(int32_t(t.I(c, "[in_r3+364]"))) > 100u ? "no" : "yes";
}

}  // namespace resolvergate
