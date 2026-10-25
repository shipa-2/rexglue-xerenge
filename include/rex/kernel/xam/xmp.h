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

}  // namespace rex::kernel::xam
