// Session 76 (a player on Discord: "add Keyboard as a Separate controller ... if I want to play with a friend, the
// Keyboard becomes Useless"; Jarrod: its own player by default, after the controllers). ReXGlue's SlotAssignment
// (device_assignment.cpp) gives pad N guest user N and merges the keyboard, a synthetic device, into user 0: with one
// pad the keyboard and the pad are the same player. This rule gives the keyboard (KEYBOARD CONTROLS on, mnk_mode) a
// guest user of its own, the lowest one no pad holds: one pad = the pad is P1 and the keyboard P2, no pad = the
// keyboard is P1. A pad that arrives on the keyboard's user moves the keyboard to the next free one; a pad that leaves
// does not pull it down (it would take over the other player's wrestler mid-match) unless no pad is left. Four pads:
// the keyboard shares P1, as before. The NOP stand-in stays on user 0 as in SlotAssignment (it keeps P1 connected
// without a pad). =false: exactly SlotAssignment.
//
// The title screen (Jarrod, round 2: "First to press Start = P1"): the game takes the controller that presses Start
// there as the profile, and ReXGlue signs in user 0 only, so Start on the keyboard as P2 gave "You are not currently
// signed in with a profile" (run 4811). Now a Start or A press from the keyboard there, while it is not P1, puts the
// keyboard first (P1, the pads after it: pad N = P(N+2)) and a pad's press puts the pads first again; the press is
// hidden from that one read, so the game sees it on P1 from the next. Only before the title's Start is accepted
// (sequence 3, boot step <= 3, states.toml seq / boot_step); after that the order stays for the session. The mouse
// (mouse_menu.h) is the keyboard's player: input_script.h's g_mouse_user.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/device.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include "input_script.h"  // g_title_pad_filter, g_mouse_user

REXCVAR_DEFINE_BOOL(wwe2k14_keyboard_own_player, true, "wwe2k14",
                    "Session 76: with KEYBOARD CONTROLS on, the keyboard is a player of its own (the first player no "
                    "controller holds: P2 with one controller, P1 with none; P1 if it presses Start first on the title "
                    "screen); =false: the keyboard and controller 1 are one player (ReXGlue's rule)");
REXCVAR_DEFINE_INT32(wwe2k14_keyboard_test_pads, 0, "wwe2k14",
                     "Session 76, tests without a controller: act as if N controllers hold P1..PN (slots only, no "
                     "device; an input script drives P1), so the keyboard takes P(N+1)");

namespace wwe2k14::keyboard_player {

using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::kMaxGuestUsers;

// The name MnkInputDriver::EnumerateDevices gives its one device (mnk_input_driver.cpp; the NOP stand-in is "None").
inline constexpr const char* kKeyboardName = "Keyboard and Mouse";

// tools\input_scripts\states.toml: seq = [[0x839400F8] + 0] + 48 (3 startup / title), boot_step = 0x8395C8D0 (3 the
// Press Start title, 4 once Start was accepted: the ATTENTION notice).
inline constexpr uint32_t kSequenceManagerPtr = 0x839400F8, kSequenceIdOffset = 48, kSequenceStartup = 3;
inline constexpr uint32_t kBootStep = 0x8395C8D0, kBootStepTitle = 3;
inline constexpr uint16_t kPadStart = 0x0010, kPadA = 0x1000;

inline std::atomic<bool> g_keyboard_first{false};  // the keyboard pressed Start / A first on the title: it is P1
inline std::atomic<uint32_t> g_order_generation{0};  // +1 on every change of g_keyboard_first
inline std::atomic<int> g_keyboard_user{-1};         // the keyboard's guest user, -1 = none

inline uint32_t Read32(uint32_t a) {
  return rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(a));
}

inline bool OnTitle() {
  const uint32_t mgr = Read32(kSequenceManagerPtr);
  const uint32_t seq = mgr ? Read32(mgr) : 0;
  return seq && Read32(seq + kSequenceIdOffset) == kSequenceStartup && Read32(kBootStep) <= kBootStepTitle;
}

class Assignment final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override {
    std::lock_guard lock(mutex_);
    devices_ = devices;
    Assign();
  }

  void DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const override {
    std::lock_guard lock(mutex_);
    if (generation_ != g_order_generation.load()) Assign();
    out.clear();
    if (user_index < kMaxGuestUsers) out = users_[user_index];
  }

 private:
  // Devices arrive sorted by ordinal (DeviceAssignment).
  void Assign() const {
    generation_ = g_order_generation.load();
    const bool first = g_keyboard_first.load();
    for (auto& user : users_) user.clear();
    std::array<bool, kMaxGuestUsers> pad_on{};
    int pads = 0;
    const DeviceInfo* keyboard = nullptr;
    for (const auto& device : devices_) {
      if (device.synthetic && device.name == kKeyboardName && REXCVAR_GET(wwe2k14_keyboard_own_player))
        keyboard = &device;
    }
    for (const auto& device : devices_) {
      if (!device.synthetic) {
        // As SlotAssignment: by ordinal, so unplugging pad one does not promote pad two. Keyboard first: one up (a
        // fourth pad has no player then).
        const uint32_t user = device.ordinal + (keyboard && first ? 1u : 0u);
        if (user < kMaxGuestUsers) {
          users_[user].push_back(device.id);
          pad_on[user] = true;
          ++pads;
        }
      } else if (&device != keyboard) {
        users_[0].push_back(device.id);
      }
    }
    const int pretend = keyboard && first ? 0 : std::clamp(int(REXCVAR_GET(wwe2k14_keyboard_test_pads)), 0,
                                                          int(kMaxGuestUsers));
    for (int i = 0; i < pretend; ++i)
      if (!pad_on[i]) pad_on[i] = true, ++pads;

    int slot = -1;
    if (keyboard) {
      if (pads == 0 || first) {
        slot = 0;
      } else if (slot_ >= 0 && !pad_on[slot_]) {
        slot = slot_;
      } else {
        for (uint32_t i = 0; i < kMaxGuestUsers && slot < 0; ++i)
          if (!pad_on[i]) slot = int(i);
        if (slot < 0) slot = 0;  // four pads: share P1
      }
      users_[slot].push_back(keyboard->id);
    }
    if (slot != slot_ || pads != pads_ || first != first_) {
      if (slot >= 0)
        REXLOG_INFO("[wwe2k14] keyboard player: P{} ({} controller(s){}{}{})", slot + 1, pads,
                    pretend ? fmt::format(", {} pretend", pretend) : std::string(),
                    first ? ", the keyboard pressed Start first" : "",
                    pad_on[slot] ? ", shared: every player slot has a controller" : "");
      else if (slot_ >= 0)
        REXLOG_INFO("[wwe2k14] keyboard player: off ({} controller(s))", pads);
    }
    slot_ = slot;
    pads_ = pads;
    first_ = first;
    g_keyboard_user = slot;
    g_mouse_user = slot > 0 ? slot : 0;
  }

  mutable std::mutex mutex_;
  std::vector<DeviceInfo> devices_;
  mutable std::array<std::vector<DeviceId>, kMaxGuestUsers> users_;
  mutable uint32_t generation_ = 0;
  mutable int slot_ = -1;  // the keyboard's guest user, -1 = none (off, shared by the setting, or not enumerated)
  mutable int pads_ = 0;
  mutable bool first_ = false;
};

// Game thread, every player's controller read (input_script.h): on the title, a Start / A press from the keyboard
// while it is not P1 puts it first; from a pad while the keyboard is first, the pads first again.
inline void TitlePadFilter(uint32_t user, rex::input::X_INPUT_STATE* state) {
  if (!(state->gamepad.buttons & (kPadStart | kPadA))) return;
  if (user == 0 || user >= kMaxGuestUsers) return;  // player 1 pressed: it is the profile already
  const int keyboard = g_keyboard_user.load();
  if (keyboard < 0 || !REXCVAR_GET(wwe2k14_keyboard_own_player)) return;
  const bool first = g_keyboard_first.load();
  if (!first && int(user) != keyboard) return;  // a second pad: ReXGlue's order, as before
  if (!OnTitle()) return;
  g_keyboard_first = !first;
  ++g_order_generation;
  const bool start = state->gamepad.buttons & kPadStart;
  state->gamepad.buttons = uint16_t(state->gamepad.buttons & ~(kPadStart | kPadA));
  REXLOG_INFO("[wwe2k14] keyboard player: {} pressed {} on the title screen (as P{}): {} first",
              first ? "a controller" : "the keyboard", start ? "Start" : "A", user + 1,
              first ? "the controllers" : "the keyboard");
}

// Replaces the SDK's SlotAssignment (CreateDefaultInputSystem) once the runtime is set up.
inline void Install(rex::Runtime* runtime) {
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  if (!input) return;
  input->SetDeviceAssignment(std::make_unique<Assignment>());
  g_title_pad_filter = TitlePadFilter;
  REXLOG_INFO("[wwe2k14] keyboard player rule installed (wwe2k14_keyboard_own_player={})",
              REXCVAR_GET(wwe2k14_keyboard_own_player));
}

}  // namespace wwe2k14::keyboard_player
