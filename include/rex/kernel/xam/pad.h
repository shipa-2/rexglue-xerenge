/**
 * @file        rex/kernel/xam/pad.h
 * @brief       The pad's state as the title reads it, seen first by the host
 *              application - to read a button the title does not use, or to
 *              press one for it.
 */
#pragma once

#include <cstdint>
#include <functional>

namespace rex::kernel::xam {

// Called with every XamInputGetState's answer before the title has it:
// `buttons` (XINPUT_GAMEPAD_*), the triggers, may be changed. Null removes it.
using PadFilter = std::function<void(uint32_t user, uint16_t& buttons, uint8_t& left_trigger,
                                     uint8_t& right_trigger)>;
void SetPadFilter(PadFilter filter);

}  // namespace rex::kernel::xam
