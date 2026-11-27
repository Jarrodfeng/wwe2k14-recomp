// S111 M100-COMP: the system headers and our headers the menu tests (menus100_tests.h) use, included at the top of equiv_tests.cpp, OUTSIDE
// namespace equiv (a header-only core of ours pulls in <mutex> and friends, which must not land inside the namespace).
#pragma once

#include <array>

#include "launch_gate.h"      // wwe2k14::mode_launch: GateRec, GateEnv, RecordGate, TargetGate, ResolverGate (Q3.5)
#include "menu_goto_core.h"   // wwe2k14::menu_goto: Rec, ReadTableWith, MarkSeen (Q5.3)
