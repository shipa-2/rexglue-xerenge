/**
 * @file        rex/kernel/xam/online.h
 * @brief       --online: where the Live services a title uses live, and the
 *              address the console presents to its peers.
 */
#pragma once

#include <cstdint>
#include <functional>

namespace rex::kernel::xam {

// The IPv4 (network byte order) of the machine standing in for a title's Xbox
// Live services - what XOnlineGetServiceInfo answers. A title that runs its
// own lobby through Live (EA's DirtySock) connects there. The provider is
// asked at every lookup and may block briefly (to find a server on the LAN);
// without one the answer is 127.0.0.1.
void SetOnlineServiceAddressProvider(std::function<uint32_t()> provider);
uint32_t OnlineServiceAddress();

// The IPv4 (network byte order) this console gives its peers in its XNADDR,
// and that the title's sockets bind to when it names no address: the
// online_address cvar when set (another loopback address, to run two copies
// on one machine), otherwise the address of the interface that reaches the
// LAN.
uint32_t OnlineLocalAddress();

}  // namespace rex::kernel::xam
