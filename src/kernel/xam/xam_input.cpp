/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

constexpr uint32_t XINPUT_FLAG_GAMEPAD = 0x01;
constexpr uint32_t XINPUT_FLAG_ANY_USER = 1 << 30;

rex::input::InputSystem* input_system() {
  return static_cast<rex::input::InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
}

void XamResetInactivity_entry() {
  // Do we need to do anything?
}

u32 XamEnableInactivityProcessing_entry(u32 unk, u32 enable) {
  return X_ERROR_SUCCESS;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetcapabilities(v=vs.85).aspx
u32 XamInputGetCapabilities_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  REXKRNL_TRACE("[XAM] XamInputGetCapabilities called: user={}, flags=0x{:X}", (uint32_t)user_index,
                (uint32_t)flags);
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  return is->GetCapabilities(actual_user_index, flags, caps);
}

u32 XamInputGetCapabilitiesEx_entry(u32 unk, u32 user_index, u32 flags,
                                    ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->GetCapabilities(actual_user_index, flags, caps);
}


namespace {

// Pad capture and replay. Reaching the screen a defect lives on can take a
// minute of menu navigation, and doing that by hand for every build is the
// slowest part of investigating one. XERENGE_CAPTURE records what was pressed
// and when; XERENGE_REPLAY feeds the same sequence back, so a run reaches the
// same place unattended.
//
// Recording happens where the title reads the pad, not where the host produces
// events, so what is written is exactly what the title saw - timing included,
// measured from the first read rather than from process start, which keeps a
// recording valid across loads of differing length.
struct PadSample {
  uint64_t at_poll = 0;
  uint64_t at_ms = 0;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
};

const char* CapturePath() {
  static const char* p = std::getenv("XERENGE_CAPTURE");
  return p;
}
const char* ReplayPath() {
  static const char* p = std::getenv("XERENGE_REPLAY");
  return p;
}

// The title polls the pad once per frame, so the poll count is its own clock:
// it advances with the game, not with the host. Wall time does not survive a
// build that renders at a different speed - the same recording then presses
// buttons on the wrong screens - while a poll index lands on the same frame
// every time.
uint64_t PadPollIndex() {
  static std::atomic<uint64_t> polls{0};
  return polls.fetch_add(1, std::memory_order_relaxed);
}

uint64_t PadClockMs() {
  using clock = std::chrono::steady_clock;
  static const auto start = clock::now();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count());
}

std::vector<PadSample>& ReplayTrack() {
  static std::vector<PadSample> track = [] {
    std::vector<PadSample> out;
    const char* path = ReplayPath();
    if (!path) {
      return out;
    }
    std::ifstream in(path);
    if (!in) {
      REXKRNL_ERROR("pad replay: cannot open {}", path);
      return out;
    }
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }
      PadSample s;
      unsigned buttons = 0, lt = 0, rt = 0;
      int lx = 0, ly = 0, rx = 0, ry = 0;
      if (std::sscanf(line.c_str(), "%llu %llu %x %u %u %d %d %d %d",
                      reinterpret_cast<unsigned long long*>(&s.at_poll),
                      reinterpret_cast<unsigned long long*>(&s.at_ms), &buttons, &lt, &rt, &lx,
                      &ly, &rx, &ry) == 9) {
        s.buttons = static_cast<uint16_t>(buttons);
        s.left_trigger = static_cast<uint8_t>(lt);
        s.right_trigger = static_cast<uint8_t>(rt);
        s.thumb_lx = static_cast<int16_t>(lx);
        s.thumb_ly = static_cast<int16_t>(ly);
        s.thumb_rx = static_cast<int16_t>(rx);
        s.thumb_ry = static_cast<int16_t>(ry);
        out.push_back(s);
      }
    }
    REXKRNL_INFO("pad replay: loaded {} sample(s) from {}", out.size(), path);
    return out;
  }();
  return track;
}

// Only a change is worth a line: the title reads the pad every frame, and a
// held button would otherwise write thousands of identical rows.
void CapturePad(const input::X_INPUT_GAMEPAD& pad, uint64_t poll) {
  const char* path = CapturePath();
  if (!path) {
    return;
  }
  static std::mutex mutex;
  static std::ofstream out;
  static PadSample last;
  static bool have_last = false;
  std::lock_guard lock(mutex);
  if (!out.is_open()) {
    out.open(path, std::ios::trunc);
    if (!out) {
      REXKRNL_ERROR("pad capture: cannot write {}", path);
      return;
    }
    out << "# poll ms buttons lt rt lx ly rx ry\n";
    REXKRNL_INFO("pad capture: writing to {}", path);
  }
  PadSample now;
  now.buttons = pad.buttons;
  now.left_trigger = pad.left_trigger;
  now.right_trigger = pad.right_trigger;
  now.thumb_lx = pad.thumb_lx;
  now.thumb_ly = pad.thumb_ly;
  now.thumb_rx = pad.thumb_rx;
  now.thumb_ry = pad.thumb_ry;
  if (have_last && now.buttons == last.buttons && now.left_trigger == last.left_trigger &&
      now.right_trigger == last.right_trigger && now.thumb_lx == last.thumb_lx &&
      now.thumb_ly == last.thumb_ly && now.thumb_rx == last.thumb_rx &&
      now.thumb_ry == last.thumb_ry) {
    return;
  }
  last = now;
  have_last = true;
  out << poll << ' ' << PadClockMs() << ' ' << std::hex << now.buttons << std::dec << ' '
      << unsigned(now.left_trigger) << ' ' << unsigned(now.right_trigger) << ' ' << now.thumb_lx
      << ' ' << now.thumb_ly << ' ' << now.thumb_rx << ' ' << now.thumb_ry << '\n';
  out.flush();
}

// The sample in force at this instant is the last one recorded at or before
// now, so a button held across several frames stays held without the recording
// having to repeat it.
bool ReplayPad(input::X_INPUT_GAMEPAD* pad, uint64_t poll) {
  auto& track = ReplayTrack();
  if (track.empty()) {
    return false;
  }
  // XERENGE_REPLAY_BY_TIME: followed by the milliseconds the recording keeps
  // beside each read rather than by the read count. With the frame rate
  // unlocked the title reads the pad every drawn frame - a thousand times a
  // second in the menus against the sixty it was recorded at - while its logic
  // still runs in real time, so the read count ran the recording through in
  // seconds and the clock keeps it on its screens.
  static const bool by_time = std::getenv("XERENGE_REPLAY_BY_TIME") != nullptr;
  const uint64_t now = by_time ? PadClockMs() : poll;
  const auto due = [](const PadSample& s) { return by_time ? s.at_ms : s.at_poll; };
  static size_t cursor = 0;
  static uint32_t delivered = 0;
  static uint64_t delivered_since = 0;
  // A press must survive the difference between the run that recorded it and
  // the run replaying it: loading takes a different number of frames, so poll
  // indices drift, and a press recorded only a few polls long can land wholly
  // inside that drift. Holding every non-neutral state for a minimum number of
  // polls makes a short tap replay as a deliberate one - the title debounces
  // anyway, so a longer press is read the same as a short one, while a press
  // too short to be seen is read as nothing at all.
  constexpr uint32_t kMinHoldPolls = 8;
  if (delivered++ == 0) {
    delivered_since = now;
  }
  // Advance by at most one sample per read. Jumping straight to the latest
  // sample due by now skips any whose successor also came due in the same
  // gap - and a press and its release are tens of milliseconds apart, so a
  // load that stalls polling for longer swallows the press whole. Stepping one
  // at a time guarantees every recorded state is handed to the title at least
  // once, which is what actually presses the button.
  const bool neutral = track[cursor].buttons == 0 && track[cursor].left_trigger == 0 &&
                       track[cursor].right_trigger == 0;
  // By time, a press is held for three logic steps however often the pad is read.
  // Only a change of buttons has to be held: a button kept down across many
  // samples while the sticks move (most of a race) put the replay seconds behind.
  const bool same_buttons = cursor + 1 < track.size() &&
                            track[cursor + 1].buttons == track[cursor].buttons &&
                            track[cursor + 1].left_trigger == track[cursor].left_trigger &&
                            track[cursor + 1].right_trigger == track[cursor].right_trigger;
  const bool held_long_enough =
      neutral || (by_time && same_buttons) ||
      (by_time ? now - delivered_since >= 50 : delivered >= kMinHoldPolls);
  if (held_long_enough && cursor + 1 < track.size() && due(track[cursor + 1]) <= now) {
    ++cursor;
    delivered = 0;
    if (by_time && track[cursor].buttons != track[cursor - 1].buttons) {
      REXKRNL_INFO("pad replay: sample {} (recorded at {} ms) buttons {:04X} given at {} ms", cursor,
                   track[cursor].at_ms, track[cursor].buttons, now);
    }
  }
  const PadSample& s = track[cursor];
  if (due(s) > now) {
    return false;
  }
  pad->buttons = s.buttons;
  pad->left_trigger = s.left_trigger;
  pad->right_trigger = s.right_trigger;
  pad->thumb_lx = s.thumb_lx;
  pad->thumb_ly = s.thumb_ly;
  pad->thumb_rx = s.thumb_rx;
  pad->thumb_ry = s.thumb_ry;
  return true;
}

// The title decides whether to look at the pad at all by watching the packet
// number: unchanged means nothing happened since the last read. Replaying
// buttons underneath a packet number that comes from an idle host pad leaves
// that number still, so presses land in a structure the title has already
// decided not to re-read - which is why some of them appeared to be dropped
// however well the timing lined up.
void StampReplayPacketNumber(input::X_INPUT_STATE* state) {
  static uint32_t packet = 0;
  static PadSample last;
  static bool have_last = false;
  PadSample now;
  now.buttons = state->gamepad.buttons;
  now.left_trigger = state->gamepad.left_trigger;
  now.right_trigger = state->gamepad.right_trigger;
  now.thumb_lx = state->gamepad.thumb_lx;
  now.thumb_ly = state->gamepad.thumb_ly;
  now.thumb_rx = state->gamepad.thumb_rx;
  now.thumb_ry = state->gamepad.thumb_ry;
  const bool changed = !have_last || now.buttons != last.buttons ||
                       now.left_trigger != last.left_trigger ||
                       now.right_trigger != last.right_trigger || now.thumb_lx != last.thumb_lx ||
                       now.thumb_ly != last.thumb_ly || now.thumb_rx != last.thumb_rx ||
                       now.thumb_ry != last.thumb_ry;
  if (changed) {
    ++packet;
    last = now;
    have_last = true;
  }
  state->packet_number = packet;
}

}  // namespace

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetstate(v=vs.85).aspx
u32 XamInputGetState_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_STATE> input_state) {
  // Games call this with a NULL state ptr, probably as a query.
  static int call_count = 0;
  if (++call_count <= 5) {
    REXKRNL_TRACE("[XAM] XamInputGetState called: user={}, flags=0x{:X}", (uint32_t)user_index,
                  (uint32_t)flags);
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  const u32 result = is->GetState(actual_user_index, input_state);
  if (result == X_ERROR_SUCCESS && input_state) {
    const uint64_t poll = PadPollIndex();
    // Live input takes over whenever there is any. A replay that cannot be
    // interrupted is only good for reaching a known screen; being able to take
    // the controls from there - without restarting, and without the recording
    // fighting back - is what makes it useful for looking at anything past it.
    // Neutral means "not touching it", so the recording continues by itself.
    const auto& live = input_state->gamepad;
    // Inside the XInput dead zones a resting pad still drifts, and that drift
    // alone used to end every replay at its first poll.
    const auto outside = [](int32_t v, int32_t zone) { return v > zone || v < -zone; };
    const bool live_active =
        (live.buttons != 0 || live.left_trigger > 30 || live.right_trigger > 30 ||
        outside(live.thumb_lx, 7849) || outside(live.thumb_ly, 7849) ||
        outside(live.thumb_rx, 8689) || outside(live.thumb_ry, 8689));
    if (live_active) {
      static bool announced = false;
      if (!announced && ReplayPath()) {
        announced = true;
        REXKRNL_INFO("pad replay: live input taking over (buttons {:04X} triggers {} {} "
                     "sticks {} {} {} {})",
                     uint32_t(live.buttons), uint32_t(live.left_trigger),
                     uint32_t(live.right_trigger), int32_t(live.thumb_lx),
                     int32_t(live.thumb_ly), int32_t(live.thumb_rx), int32_t(live.thumb_ry));
      }
      StampReplayPacketNumber(&*input_state);
      // Record what the title saw, even mid-replay. Extending a recording by
      // steering on top of it is the only way to reach a screen the recording
      // stops short of without walking the whole route by hand again, and the
      // file that comes out is a complete route rather than a fragment.
      CapturePad(input_state->gamepad, poll);
      return X_ERROR_SUCCESS;
    }
    if (ReplayPad(&input_state->gamepad, poll)) {
      StampReplayPacketNumber(&*input_state);
      CapturePad(input_state->gamepad, poll);
      return X_ERROR_SUCCESS;
    }
    CapturePad(input_state->gamepad, poll);
  }
  return result;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputsetstate(v=vs.85).aspx
u32 XamInputSetState_entry(u32 user_index, u32 unk, ppc_ptr_t<X_INPUT_VIBRATION> vibration) {
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  uint32_t actual_user_index = user_index;
  if ((user_index & 0xFF) == 0xFF) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->SetState(actual_user_index, vibration);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetkeystroke(v=vs.85).aspx
u32 XamInputGetKeystroke_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  // https://github.com/CodeAsm/ffplay360/blob/master/Common/AtgXime.cpp
  // user index = index or XUSER_INDEX_ANY
  // flags = XINPUT_FLAG_GAMEPAD (| _ANYUSER | _ANYDEVICE)

  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  return is->GetKeystroke(actual_user_index, flags, keystroke);
}

// Same as non-ex, just takes a pointer to user index.
u32 XamInputGetKeystrokeEx_entry(mapped_u32 user_index_ptr, u32 flags,
                                 ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t user_index = *user_index_ptr;
  if ((user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    user_index = 0;
  }

  auto* is = input_system();
  auto result = is->GetKeystroke(user_index, flags, keystroke);
  if (XSUCCEEDED(result)) {
    *user_index_ptr = keystroke->user_index;
  }
  return result;
}

i32 XamUserGetDeviceContext_entry(u32 user_index, u32 unk, mapped_u32 out_ptr) {
  // Games check the result - usually with some masking.
  // If this function fails they assume zero, so let's fail AND
  // set zero just to be safe.
  *out_ptr = 0;
  if (!user_index || (user_index & 0xFF) == 0xFF) {
    return X_E_SUCCESS;
  } else {
    return X_E_DEVICE_NOT_CONNECTED;
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamResetInactivity, rex::kernel::xam::XamResetInactivity_entry)
REX_EXPORT(__imp__XamEnableInactivityProcessing,
           rex::kernel::xam::XamEnableInactivityProcessing_entry)
REX_EXPORT(__imp__XamInputGetCapabilities, rex::kernel::xam::XamInputGetCapabilities_entry)
REX_EXPORT(__imp__XamInputGetCapabilitiesEx, rex::kernel::xam::XamInputGetCapabilitiesEx_entry)
REX_EXPORT(__imp__XamInputGetState, rex::kernel::xam::XamInputGetState_entry)
REX_EXPORT(__imp__XamInputSetState, rex::kernel::xam::XamInputSetState_entry)
REX_EXPORT(__imp__XamInputGetKeystroke, rex::kernel::xam::XamInputGetKeystroke_entry)
REX_EXPORT(__imp__XamInputGetKeystrokeEx, rex::kernel::xam::XamInputGetKeystrokeEx_entry)
REX_EXPORT(__imp__XamUserGetDeviceContext, rex::kernel::xam::XamUserGetDeviceContext_entry)

REX_EXPORT_STUB(__imp__XamInputControl);
REX_EXPORT_STUB(__imp__XamInputEnableAutobind);
REX_EXPORT_STUB(__imp__XamInputGetDeviceStats);
REX_EXPORT_STUB(__imp__XamInputGetFailedConnectionOrBind);
REX_EXPORT_STUB(__imp__XamInputGetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHud);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHudEx);
REX_EXPORT_STUB(__imp__XamInputGetUserVibrationLevel);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRawEx);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRawEx);
REX_EXPORT_STUB(__imp__XamInputRawState);
REX_EXPORT_STUB(__imp__XamInputResetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSendStayAliveRequest);
REX_EXPORT_STUB(__imp__XamInputSendXenonButtonPress);
REX_EXPORT_STUB(__imp__XamInputSetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputSetKeyboardTranslationHud);
REX_EXPORT_STUB(__imp__XamInputSetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSetMinMaxAuthDelay);
REX_EXPORT_STUB(__imp__XamInputSetTextMessengerIndicator);
REX_EXPORT_STUB(__imp__XamInputToggleKeyLocks);
