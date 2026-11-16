/**
 * @file        rex/kernel/xam/xmp.h
 * @brief       The title's soundtrack (XMP): where its songs are read from.
 */
#pragma once

#include <functional>
#include <string>

namespace rex::kernel::xam {

// A song's file as the title names it (its playlist's path, such as
// GAME:\eatrax\07.wma) -> the path XMP reads instead, in the same virtual
// file system; the same path to read it as it is. Asked each time a song
// starts, so what it answers may change while the title plays.
void SetXmpSongPathResolver(std::function<std::string(const std::string&)> resolver);

// A pause the title asks for may be held instead of taken: while held the
// music keeps playing; a continue/stop/play before the hold expires discards
// it (a short camera's stop-and-resume), otherwise the pause lands when the
// hold ends. Xerenge's XERENGE_CRASH_MUSIC and XERENGE_TAKEDOWN_MUSIC hold
// pauses this way to keep the soundtrack through crash and takedown cameras.
// `hold_pause` is asked for every pause; the hold lasts `timeout_ms`.
void SetXmpPauseHold(std::function<bool()> hold_pause, uint64_t timeout_ms = 6000);

}  // namespace rex::kernel::xam
