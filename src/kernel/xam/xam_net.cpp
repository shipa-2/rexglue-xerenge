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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <array>
#include <cerrno>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <cstring>

#if REX_PLATFORM_MAC
#include <sys/select.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/online.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#include <ws2tcpip.h>                    // inet_pton, inet_ntop
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// --online: answer as a console signed in to Xbox Live with a working
// connection would. Nothing here reaches a real service - it only gets a title
// past the checks in front of its online menus. Off, everything is as before:
// no cable, not signed in to Live.
REXCVAR_DEFINE_BOOL(online, false, "Network",
                    "Pretend to be signed in to Xbox Live with a working connection (a stub for "
                    "reaching online menus; no real service is contacted)");
REXCVAR_DEFINE_STRING(online_address, "", "Network",
                      "With --online: the IPv4 this console gives its peers and binds its "
                      "sockets to (e.g. 127.0.0.2 and 127.0.0.3 to run two copies on one "
                      "machine). Empty: the address of the interface that reaches the LAN");

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// Set by --online (the cvar is defined above, outside the namespace, so the
// title's own code can read it too).
bool OnlineStub() {
  return REXCVAR_GET(online);
}

namespace {
std::mutex g_online_mutex;
// A function-local static: the title's side may register its provider from its
// own static initialisers, which can run before this file's.
std::function<uint32_t()>& ServiceAddressProvider() {
  static std::function<uint32_t()> provider;
  return provider;
}
}  // namespace

void SetOnlineServiceAddressProvider(std::function<uint32_t()> provider) {
  std::lock_guard lock(g_online_mutex);
  ServiceAddressProvider() = std::move(provider);
}

uint32_t OnlineServiceAddress() {
  std::function<uint32_t()> provider;
  {
    std::lock_guard lock(g_online_mutex);
    provider = ServiceAddressProvider();
  }
  const uint32_t address = provider ? provider() : 0;
  return address ? address : htonl(INADDR_LOOPBACK);
}

uint32_t OnlineLocalAddress() {
  static const uint32_t address = [] {
    const std::string configured = REXCVAR_GET(online_address);
    if (!configured.empty()) {
      in_addr parsed{};
      if (inet_pton(AF_INET, configured.c_str(), &parsed) == 1) {
        return uint32_t(parsed.s_addr);
      }
      REXKRNL_WARN("--online: online_address '{}' is not an IPv4 address", configured);
    }
#if !REX_PLATFORM_WIN32
    // The LAN address of a physical interface (wired or wireless): not the
    // default route's, which on a machine with a VPN or a proxy's TUN device
    // is that device, unreachable from the next machine on the LAN.
    {
      ifaddrs* interfaces = nullptr;
      uint32_t best = 0;
      int best_rank = 0;
      if (getifaddrs(&interfaces) == 0) {
        for (ifaddrs* i = interfaces; i; i = i->ifa_next) {
          if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) ||
              (i->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT))) {
            continue;
          }
          const std::string name = i->ifa_name ? i->ifa_name : "";
          const auto starts = [&](const char* prefix) { return name.rfind(prefix, 0) == 0; };
          if (starts("docker") || starts("br-") || starts("veth") || starts("virbr") ||
              starts("tun") || starts("tap") || starts("wg") || starts("tailscale") ||
              starts("zt") || starts("singbox") || starts("vmnet") || starts("vboxnet")) {
            continue;
          }
          const uint32_t address = reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr;
          const uint32_t host = ntohl(address);
          const bool private_range = (host >> 24) == 10 || (host >> 20) == 0xAC1 ||
                                     (host >> 16) == 0xC0A8;
          const bool physical = starts("en") || starts("eth") || starts("wl");
          const int rank = (private_range ? 2 : 0) + (physical ? 1 : 0) + 1;
          if (rank > best_rank) {
            best_rank = rank;
            best = address;
          }
        }
        freeifaddrs(interfaces);
      }
      if (best) {
        char text[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &best, text, sizeof(text));
        REXKRNL_INFO("--online: this console's address is {}", text);
        return best;
      }
    }
#endif
    // The interface a packet to the outside would leave by. A UDP "connect"
    // only picks the route; nothing is sent.
#if REX_PLATFORM_WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    uint32_t found = htonl(INADDR_LOOPBACK);
    const auto probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);  // TEST-NET-1, never answered
    if (::connect(probe, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
      sockaddr_in self{};
#if REX_PLATFORM_WIN32
      int self_len = sizeof(self);
#else
      socklen_t self_len = sizeof(self);
#endif
      if (::getsockname(probe, reinterpret_cast<sockaddr*>(&self), &self_len) == 0 &&
          self.sin_addr.s_addr != 0) {
        found = self.sin_addr.s_addr;
      }
    }
#if REX_PLATFORM_WIN32
    closesocket(probe);
#else
    ::close(probe);
#endif
    char text[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &found, text, sizeof(text));
    REXKRNL_INFO("--online: this console's address is {}", text);
    return found;
  }();
  return address;
}

namespace {
// The XNADDRs the title has turned into IN_ADDRs, by the IN_ADDR, for the
// way back. An IN_ADDR is the peer's real IPv4 here.
std::map<uint32_t, std::array<uint8_t, 36>> g_known_xnaddrs;

// Distinct per console: peers tell each other apart by these, and two
// copies of the game must not look like one.
void FillConsoleIdentity(uint32_t address, uint8_t enet[6], uint8_t online[20]) {
  const uint8_t* ip = reinterpret_cast<const uint8_t*>(&address);
  enet[0] = 0x02;  // locally administered
  enet[1] = 0x58;
  for (int i = 0; i < 4; ++i) {
    enet[2 + i] = ip[i];
  }
  for (uint8_t i = 0; i < 20; ++i) {
    online[i] = uint8_t(0x5A + i) ^ ip[i % 4];
  }
}
}  // namespace

namespace {
// --online: what the title says on the wire, to learn the protocols it talks
// (EA's Aries lobby among them). At most 256 bytes a call, as text where it is
// printable and \xNN where it is not.
void LogWire(const char* what, uint32_t socket, const uint8_t* data, int length) {
  if (!OnlineStub() || length <= 0 || !data) {
    return;
  }
  std::string text;
  const int shown = std::min(length, 256);
  for (int i = 0; i < shown; ++i) {
    const uint8_t c = data[i];
    if (c >= 0x20 && c < 0x7F && c != '\\') {
      text += char(c);
    } else {
      char hex[5];
      std::snprintf(hex, sizeof(hex), "\\x%02X", c);
      text += hex;
    }
  }
  REXKRNL_INFO("--online wire: {} socket {:08X} {} byte(s){}: {}", what, socket, length,
               length > shown ? " (first 256)" : "", text);
}
}  // namespace

// The last socket error, as the title's WinSock would report it. On Linux the
// host reports it in errno, in its own numbering; without this the title saw
// either "no error" or a generic failure - a non-blocking connect in progress
// read as a failed one, and "no data yet" from recv as a broken connection.
void SetLastSocketError() {
#if REX_PLATFORM_WIN32
  XThread::SetLastError(WSAGetLastError());
#else
  uint32_t wsa;
  switch (errno) {
    case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
    case EAGAIN:
#endif
    case EINPROGRESS:
      wsa = 10035;  // WSAEWOULDBLOCK (what a non-blocking connect returns too)
      break;
    case EALREADY: wsa = 10037; break;       // WSAEALREADY
    case ENOTSOCK: wsa = 10038; break;       // WSAENOTSOCK
    case EMSGSIZE: wsa = 10040; break;       // WSAEMSGSIZE
    case EADDRINUSE: wsa = 10048; break;     // WSAEADDRINUSE
    case EADDRNOTAVAIL: wsa = 10049; break;  // WSAEADDRNOTAVAIL
    case ENETDOWN: wsa = 10050; break;       // WSAENETDOWN
    case ENETUNREACH: wsa = 10051; break;    // WSAENETUNREACH
    case ECONNABORTED: wsa = 10053; break;   // WSAECONNABORTED
    case ECONNRESET: wsa = 10054; break;     // WSAECONNRESET
    case ENOBUFS: wsa = 10055; break;        // WSAENOBUFS
    case EISCONN: wsa = 10056; break;        // WSAEISCONN
    case ENOTCONN: wsa = 10057; break;       // WSAENOTCONN
    case ETIMEDOUT: wsa = 10060; break;      // WSAETIMEDOUT
    case ECONNREFUSED: wsa = 10061; break;   // WSAECONNREFUSED
    case EHOSTUNREACH: wsa = 10065; break;   // WSAEHOSTUNREACH
    case EINVAL: wsa = 10022; break;         // WSAEINVAL
    default: wsa = 10050; break;
  }
  XThread::SetLastError(wsa);
#endif
}

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
typedef struct {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;                    // IP address (zero if not static/DHCP)
  in_addr inaOnline;              // Online IP address (zero if not online)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
} XNADDR;

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

typedef struct {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
} XNQOSINFO;

typedef struct {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
} XNQOS;

struct Xsockaddr_t {
  rex::be<uint16_t> sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = memory::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = memory::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      memory::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      memory::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      memory::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

XNetStartupParams xnet_startup_params = {};

u32 NetDll_XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return 0;
}

u32 NetDll_XNetCleanup_entry(u32 caller, mapped_void params) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  // For now, constant values.
  // This makes replicating things easier.
  std::memset(buffer_ptr, 0xBB, length);

  return 0;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version;
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  // DEBUG
  /*
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}

u32 NetDll_WSACleanup_entry(u32 caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket, ppc_ptr_t<XWSABUF> buffers_ptr,
                             u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                             ppc_ptr_t<XSOCKADDR_IN> from_addr,
                             ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                             mapped_void completion_routine_ptr) {
  if (overlapped_ptr) {
    // auto evt = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(
    //    overlapped_ptr->event_handle);

    // if (evt) {
    //  //evt->Set(0, false);
    //}
  }

  // we're not going to be receiving packets any time soon
  // return error so we don't wait on that - Cancerous
  return -1;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                           u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  assert(!overlapped);
  assert(!completion_routine);

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // Our sockets implementation doesn't support multiple buffers, so we need
  // to combine the buffers the game has given us!
  std::vector<uint8_t> combined_buffer_mem;
  uint32_t combined_buffer_size = 0;
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    uint8_t* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  if (OnlineStub()) {
    const uint32_t ip = native_to.sin_addr;
    REXKRNL_INFO("--online wire: sendto socket {:08X} to {}.{}.{}.{}:{}, {} byte(s)", socket_handle,
                 ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
                 uint16_t(native_to.sin_port), combined_buffer_size);
  }
  socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);

  // TODO: Instantly complete overlapped

  return 0;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  uint64_t timeout_wait = (uint64_t)timeout;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, wait_all, 1, alertable,
                                                    timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  return 0;
}

u32 NetDll_WSACreateEvent_entry() {
  auto ev = object_ref<XEvent>(new XEvent(REX_KERNEL_STATE()));
  ev->Initialize(true, false);
  return ev->handle();
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static const uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  if (OnlineStub()) {
    // A console with a static address behind a gateway, DNS configured and
    // online. EA's DirtySock reads GATEWAY/DNS as "+isp" - an internet
    // connection - and anything less as a problem to report. The address is
    // the real one, so that peers who are handed this XNADDR (through a
    // lobby) can reach this console directly.
    const uint32_t address = OnlineLocalAddress();
    addr_ptr->ina.s_addr = address;
    addr_ptr->inaOnline.s_addr = address;
    addr_ptr->wPortOnline = 3074;
    FillConsoleIdentity(address, addr_ptr->abEnet, addr_ptr->abOnline);
    return XnAddrStatus::XNET_GET_XNADDR_ETHERNET | XnAddrStatus::XNET_GET_XNADDR_STATIC |
           XnAddrStatus::XNET_GET_XNADDR_GATEWAY | XnAddrStatus::XNET_GET_XNADDR_DNS |
           XnAddrStatus::XNET_GET_XNADDR_ONLINE;
  }
  // Just return a loopback address atm.
  addr_ptr->ina.s_addr = htonl(INADDR_LOOPBACK);
  addr_ptr->inaOnline.s_addr = 0;
  addr_ptr->wPortOnline = 0;

  // TODO(gibbed): A proper mac address.
  // RakNet's 360 version appears to depend on abEnet to create "random" 64-bit
  // numbers. A zero value will cause RakPeer::Startup to fail. This causes
  // 58411436 to crash on startup.
  // The 360-specific code is scrubbed from the RakNet repo, but there's still
  // traces of what it's doing which match the game code.
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L382
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4527
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4467
  // "Mac address is a poor solution because you can't have multiple connections
  // from the same system"
  std::memset(addr_ptr->abEnet, 0xCC, 6);

  std::memset(addr_ptr->abOnline, 0, 20);

  return XnAddrStatus::XNET_GET_XNADDR_STATIC;
}

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr, mapped_u32 id_ptr) {
  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}

void NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, mapped_string string_out,
                                     u32 string_size) {
  if (OnlineStub()) {
    // in_addr arrives as the guest's big-endian word, which is network order.
    const uint32_t network = htonl(in_addr);
    char text[INET_ADDRSTRLEN] = "0.0.0.0";
    inet_ntop(AF_INET, &network, text, sizeof(text));
    rex::string::copy_truncating(string_out, text, string_size);
    return;
  }
  rex::string::copy_truncating(string_out, "666.666.666.666", string_size);
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address). On a console it is
// an opaque handle into the secure-connection table; here it is the peer's
// real IPv4, which the sockets can send to as they are.
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                    mapped_void in_addr) {
  if (OnlineStub() && in_addr && xn_addr) {
    uint32_t address = xn_addr->ina.s_addr ? uint32_t(xn_addr->ina.s_addr)
                                           : uint32_t(xn_addr->inaOnline.s_addr);
    if (!address) {
      address = htonl(INADDR_LOOPBACK);
    }
    {
      std::lock_guard lock(g_online_mutex);
      std::array<uint8_t, 36> copy;
      std::memcpy(copy.data(), static_cast<XNADDR*>(xn_addr), sizeof(XNADDR));
      g_known_xnaddrs[address] = copy;
    }
    REXKRNL_INFO("--online wire: XNetXnAddrToInAddr -> {:08X}", htonl(address));
    std::memcpy(in_addr, &address, sizeof(address));
    return 0;
  }
  return 1;
}

// Does the reverse of the above: the XNADDR an IN_ADDR came from.
u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, u32 in_addr, ppc_ptr_t<XNADDR> xn_addr,
                                    mapped_void xid) {
  if (!OnlineStub() || !xn_addr) {
    return 1;
  }
  const uint32_t address = htonl(in_addr);
  std::lock_guard lock(g_online_mutex);
  const auto it = g_known_xnaddrs.find(address);
  if (it != g_known_xnaddrs.end()) {
    std::memcpy(static_cast<XNADDR*>(xn_addr), it->second.data(), sizeof(XNADDR));
  } else {
    // Never handed over: an address with nothing else known about it.
    std::memset(static_cast<XNADDR*>(xn_addr), 0, sizeof(XNADDR));
    xn_addr->ina.s_addr = address;
    xn_addr->inaOnline.s_addr = address;
    xn_addr->wPortOnline = 3074;
    FillConsoleIdentity(address, xn_addr->abEnet, xn_addr->abOnline);
  }
  if (xid) {
    std::memset(xid, 0, 8);
  }
  return 0;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
u32 NetDll_XNetSetSystemLinkPort_entry(u32 caller, u32 port) {
  return 1;
}

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static const uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

// Opening a secure connection to a peer or a service. Only --online answers:
// without it these stay what the old stubs returned (the caller argument, 1 -
// an error to XNetConnect, "pending" to XNetGetConnectStatus).
u32 NetDll_XNetConnect_entry(u32 caller, u32 in_addr) {
  if (OnlineStub()) {
    REXKRNL_INFO("--online wire: XNetConnect {:08X}", in_addr);
  }
  return OnlineStub() ? 0 : 1;
}

u32 NetDll_XNetGetConnectStatus_entry(u32 caller, u32 in_addr) {
  if (OnlineStub()) {
    REXKRNL_INFO("--online wire: XNetGetConnectStatus {:08X}", in_addr);
  }
  constexpr uint32_t kConnectStatusConnected = 2;  // XNET_CONNECT_STATUS_CONNECTED
  return OnlineStub() ? kConnectStatusConnected : 1;
}

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  if (OnlineStub()) {
    return XEthernetStatus::XNET_ETHERNET_LINK_ACTIVE | XEthernetStatus::XNET_ETHERNET_LINK_100MBPS |
           XEthernetStatus::XNET_ETHERNET_LINK_FULL_DUPLEX;
  }
  return 0;
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle, mapped_u32 pdns) {
  if (OnlineStub()) {
    REXKRNL_INFO("--online wire: DNS lookup of '{}' (answered: not found)",
                 host ? std::string(host.host_address()) : std::string("?"));
  }
  // TODO(gibbed): actually implement this
  if (pdns) {
    auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
    dns->status = 1;  // non-zero = error
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetDnsRelease_entry(u32 caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
  // Set pqos as some games will try accessing it despite non-successful result
  if (pqos) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

// --online QoS. On a console XNetQosLookup probes a peer and brings back
// whatever the peer published with XNetQosListen for that session key -
// titles pass session details that way. Here each copy of the game answers
// such probes on UDP 3075 (on its online_address) with what it published:
//   probe  "XQS?" + XNKID (8 bytes)
//   answer "XQS!" + XNKID + the published data
namespace {
constexpr uint16_t kQosPort = 3075;
constexpr uint8_t kQosInfoComplete = 0x01;
constexpr uint8_t kQosInfoTargetContacted = 0x02;
constexpr uint8_t kQosInfoDataReceived = 0x08;
constexpr uint32_t kQosListenEnable = 0x01;
constexpr uint32_t kQosListenDisable = 0x02;
constexpr uint32_t kQosListenSetData = 0x04;
constexpr uint32_t kQosListenRelease = 0x10;

struct QosListener {
  bool enabled = false;
  std::vector<uint8_t> data;
};
std::map<uint64_t, QosListener> g_qos_listeners;  // by XNKID; g_online_mutex

#if REX_PLATFORM_WIN32
using NativeSocket = SOCKET;
void CloseNative(NativeSocket s) { closesocket(s); }
bool NativeValid(NativeSocket s) { return s != INVALID_SOCKET; }
#else
using NativeSocket = int;
void CloseNative(NativeSocket s) { ::close(s); }
bool NativeValid(NativeSocket s) { return s >= 0; }
#endif

// A guest buffer's bytes.
uint8_t* Bytes(mapped_void p) {
  return static_cast<uint8_t*>(static_cast<void*>(p));
}

uint64_t KidKey(const uint8_t* kid) {
  uint64_t key = 0;
  std::memcpy(&key, kid, 8);
  return key;
}

// The address the QoS socket and the title's unaddressed binds use: the
// configured one, or any when the console just uses its LAN address.
uint32_t BindAddress() {
  return REXCVAR_GET(online_address).empty() ? htonl(INADDR_ANY) : OnlineLocalAddress();
}

void StartQosResponder() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      const NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      sockaddr_in at{};
      at.sin_family = AF_INET;
      at.sin_port = htons(kQosPort);
      at.sin_addr.s_addr = BindAddress();
      if (!NativeValid(s) || ::bind(s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0) {
        REXKRNL_WARN("--online: QoS responder cannot bind port {}; peers will get no QoS data",
                     kQosPort);
        if (NativeValid(s)) {
          CloseNative(s);
        }
        return;
      }
      REXKRNL_INFO("--online: answering QoS probes on UDP {}", kQosPort);
      for (;;) {
        uint8_t in[64];
        sockaddr_in from{};
#if REX_PLATFORM_WIN32
        int from_len = sizeof(from);
#else
        socklen_t from_len = sizeof(from);
#endif
        const int got = int(::recvfrom(s, reinterpret_cast<char*>(in), sizeof(in), 0,
                                       reinterpret_cast<sockaddr*>(&from), &from_len));
        if (got < 12 || std::memcmp(in, "XQS?", 4) != 0) {
          continue;
        }
        std::vector<uint8_t> out(in, in + 12);
        std::memcpy(out.data(), "XQS!", 4);
        {
          std::lock_guard lock(g_online_mutex);
          const auto it = g_qos_listeners.find(KidKey(in + 4));
          if (it == g_qos_listeners.end() || !it->second.enabled) {
            continue;
          }
          out.insert(out.end(), it->second.data.begin(), it->second.data.end());
        }
        ::sendto(s, reinterpret_cast<const char*>(out.data()), int(out.size()), 0,
                 reinterpret_cast<sockaddr*>(&from), from_len);
      }
    }).detach();
  });
}
}  // namespace

u32 NetDll_XNetQosListen_entry(u32 caller, mapped_void id, mapped_void data, u32 data_size, u32 r7,
                               u32 flags) {
  if (!OnlineStub()) {
    return X_ERROR_FUNCTION_FAILED;
  }
  if (!id) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const uint64_t key = KidKey(Bytes(id));
  {
    std::lock_guard lock(g_online_mutex);
    if (flags & kQosListenRelease) {
      g_qos_listeners.erase(key);
    } else {
      QosListener& listener = g_qos_listeners[key];
      if (flags & kQosListenSetData) {
        const uint8_t* bytes = Bytes(data);
        listener.data.assign(bytes, bytes + (bytes ? data_size : 0));
      }
      if (flags & kQosListenEnable) {
        listener.enabled = true;
      }
      if (flags & kQosListenDisable) {
        listener.enabled = false;
      }
    }
  }
  REXKRNL_INFO("--online: QoS listen flags {:02X}, {} byte(s) of data", uint32_t(flags),
               uint32_t(data_size));
  StartQosResponder();
  return 0;
}

// Probes each peer (and answers for each service) at once, on its own thread,
// and fills the XNQOS the title polls; the event is set when all are in. A
// peer that does not answer is still reported reachable, without data - an
// older copy may simply not run the responder.
u32 NetDll_XNetQosLookup_entry(u32 caller, u32 xnaddr_count, mapped_u32 xnaddrs, mapped_u32 xnkids,
                               mapped_u32 xnkeys, u32 service_count, mapped_u32 service_addrs,
                               mapped_u32 service_ids, u32 probes, u32 bits_per_sec, u32 flags,
                               u32 event_handle, mapped_u32 pqos) {
  if (!OnlineStub()) {
    return X_ERROR_FUNCTION_FAILED;
  }
  const uint32_t count = xnaddr_count + service_count;
  const uint32_t qos_size = 8 + std::max<uint32_t>(count, 1) * sizeof(XNQOSINFO);
  const uint32_t qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(qos_size);
  auto* qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
  std::memset(qos, 0, qos_size);
  qos->count = count;
  qos->count_pending = count;
  if (pqos) {
    *pqos = qos_guest;
  }

  struct Target {
    uint32_t address = 0;
    uint64_t kid = 0;
  };
  std::vector<Target> targets;
  auto* memory = REX_KERNEL_MEMORY();
  for (uint32_t i = 0; i < xnaddr_count; ++i) {
    Target target;
    const uint32_t xnaddr_guest = xnaddrs ? uint32_t(xnaddrs[i]) : 0;
    if (xnaddr_guest) {
      const auto* xnaddr = memory->TranslateVirtual<const XNADDR*>(xnaddr_guest);
      target.address = xnaddr->ina.s_addr ? uint32_t(xnaddr->ina.s_addr)
                                          : uint32_t(xnaddr->inaOnline.s_addr);
    }
    const uint32_t kid_guest = xnkids ? uint32_t(xnkids[i]) : 0;
    if (kid_guest) {
      target.kid = KidKey(memory->TranslateVirtual<const uint8_t*>(kid_guest));
    }
    targets.push_back(target);
  }
  REXKRNL_INFO("--online: QoS lookup of {} peer(s) and {} service(s)", uint32_t(xnaddr_count),
               uint32_t(service_count));

  std::thread([qos_guest, targets, count, event_handle] {
    auto* memory = REX_KERNEL_MEMORY();
    struct Result {
      bool answered = false;
      uint16_t rtt = 0;
      std::vector<uint8_t> data;
    };
    std::vector<Result> results(targets.size());
    const NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (NativeValid(s)) {
      sockaddr_in from_at{};
      from_at.sin_family = AF_INET;
      from_at.sin_addr.s_addr = BindAddress();
      ::bind(s, reinterpret_cast<sockaddr*>(&from_at), sizeof(from_at));
      const auto sent = std::chrono::steady_clock::now();
      for (const Target& target : targets) {
        if (!target.address) {
          continue;
        }
        uint8_t probe[12];
        std::memcpy(probe, "XQS?", 4);
        std::memcpy(probe + 4, &target.kid, 8);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(kQosPort);
        to.sin_addr.s_addr = target.address;
        ::sendto(s, reinterpret_cast<const char*>(probe), sizeof(probe), 0,
                 reinterpret_cast<sockaddr*>(&to), sizeof(to));
      }
      const auto deadline = sent + std::chrono::milliseconds(600);
      for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          break;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        const auto left =
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
        timeval wait{long(left / 1000000), long(left % 1000000)};
        if (::select(int(s) + 1, &readable, nullptr, nullptr, &wait) <= 0) {
          break;
        }
        uint8_t in[1500];
        sockaddr_in from{};
#if REX_PLATFORM_WIN32
        int from_len = sizeof(from);
#else
        socklen_t from_len = sizeof(from);
#endif
        const int got = int(::recvfrom(s, reinterpret_cast<char*>(in), sizeof(in), 0,
                                       reinterpret_cast<sockaddr*>(&from), &from_len));
        if (got < 12 || std::memcmp(in, "XQS!", 4) != 0) {
          continue;
        }
        const uint16_t rtt = uint16_t(std::max<int64_t>(
            1, std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - sent)
                   .count()));
        for (size_t i = 0; i < targets.size(); ++i) {
          if (!results[i].answered && targets[i].address == from.sin_addr.s_addr &&
              targets[i].kid == KidKey(in + 4)) {
            results[i].answered = true;
            results[i].rtt = rtt;
            results[i].data.assign(in + 12, in + got);
            break;
          }
        }
      }
      CloseNative(s);
    }

    auto* qos = memory->TranslateVirtual<XNQOS*>(qos_guest);
    for (uint32_t i = 0; i < count; ++i) {
      XNQOSINFO& info = qos->info[i];
      uint8_t info_flags = kQosInfoComplete | kQosInfoTargetContacted;
      uint16_t rtt = 20;
      if (i < results.size() && results[i].answered) {
        rtt = results[i].rtt;
        if (!results[i].data.empty()) {
          const uint32_t data_guest = memory->SystemHeapAlloc(uint32_t(results[i].data.size()));
          std::memcpy(memory->TranslateVirtual<uint8_t*>(data_guest), results[i].data.data(),
                      results[i].data.size());
          info.data_ptr = data_guest;
          info.data_len = uint16_t(results[i].data.size());
          info_flags |= kQosInfoDataReceived;
        }
      }
      info.probes_xmit = 4;
      info.probes_recv = 4;
      info.rtt_min_in_msecs = rtt;
      info.rtt_med_in_msecs = rtt;
      info.up_bits_per_sec = 10'000'000;
      info.down_bits_per_sec = 10'000'000;
      info.flags = info_flags;
    }
    qos->count_pending = 0;
    uint32_t answered = 0;
    for (const Result& result : results) {
      answered += result.answered ? 1 : 0;
    }
    REXKRNL_INFO("--online: QoS lookup done, {} of {} peer(s) answered", answered,
                 uint32_t(targets.size()));
    if (event_handle) {
      if (auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle)) {
        ev->Set(0, false);
      }
    }
  }).detach();
  return 0;
}

// Session keys. An online peer-to-peer key (XNKID's top bits 0x80, as
// XNetXnKidIsOnlinePeer checks) with a random body; nothing is encrypted.
u32 NetDll_XNetCreateKey_entry(u32 caller, mapped_void xnkid, mapped_void xnkey) {
  if (!OnlineStub()) {
    return 1;
  }
  static std::mt19937_64 random{std::random_device{}()};
  std::lock_guard lock(g_online_mutex);
  if (xnkid) {
    uint8_t* kid = Bytes(xnkid);
    for (int i = 0; i < 8; ++i) {
      kid[i] = uint8_t(random());
    }
    kid[0] = uint8_t((kid[0] & 0x0F) | 0x80);
  }
  if (xnkey) {
    uint8_t* key = Bytes(xnkey);
    for (int i = 0; i < 16; ++i) {
      key[i] = uint8_t(random());
    }
  }
  return 0;
}

// XSessionCreate as a host: the title reads its XSESSION_INFO back - the
// session's key id (an online peer XNKID, as XNetCreateKey makes), this
// console's XNADDR (as XNetGetTitleXnAddr gives it) and the key exchange key -
// and hands it to the lobby, for the players who join to connect with. Left
// zero, Burnout gave up on the game it had just created ("the game you were
// in no longer exists").
bool FillHostSessionInfo(uint8_t* info, uint8_t* nonce) {
  if (!OnlineStub()) {
    return false;
  }
  static std::mt19937_64 random{std::random_device{}()};
  std::lock_guard lock(g_online_mutex);
  if (info) {
    uint8_t* kid = info;
    for (int i = 0; i < 8; ++i) {
      kid[i] = uint8_t(random());
    }
    kid[0] = uint8_t((kid[0] & 0x0F) | 0x80);
    auto* host = reinterpret_cast<XNADDR*>(info + 8);
    std::memset(host, 0, sizeof(XNADDR));
    const uint32_t address = OnlineLocalAddress();
    host->ina.s_addr = address;
    host->inaOnline.s_addr = address;
    host->wPortOnline = 3074;
    FillConsoleIdentity(address, host->abEnet, host->abOnline);
    uint8_t* key = info + 8 + sizeof(XNADDR);
    for (int i = 0; i < 16; ++i) {
      key[i] = uint8_t(random());
    }
  }
  if (nonce) {
    for (int i = 0; i < 8; ++i) {
      nonce[i] = uint8_t(random());
    }
  }
  return true;
}

u32 NetDll_XNetRegisterKey_entry(u32 caller, mapped_void xnkid, mapped_void xnkey) {
  return OnlineStub() ? 0 : 1;
}

u32 NetDll_XNetUnregisterKey_entry(u32 caller, mapped_void xnkid) {
  return OnlineStub() ? 0 : 1;
}

u32 NetDll_XNetUnregisterInAddr_entry(u32 caller, u32 in_addr) {
  return OnlineStub() ? 0 : 1;
}

// A title server's address as a secure-connection handle: the address itself.
u32 NetDll_XNetServerToInAddr_entry(u32 caller, u32 in_addr, u32 service_id, mapped_u32 pina) {
  if (!OnlineStub() || !pina) {
    return 1;
  }
  *pina = in_addr;
  return 0;
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
  if (OnlineStub()) {
    REXKRNL_INFO("--online wire: socket af {} type {} protocol {}", af, type, protocol);
  }
  auto socket = object_ref<XSocket>(new XSocket(REX_KERNEL_STATE()));
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->ReleaseHandle();

    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return -1;
  }

  return socket->handle();
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
SetLastSocketError();
  }
  return ret;
}

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  return XSUCCEEDED(status) ? 0 : -1;
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_void arg_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (OnlineStub()) {
    REXKRNL_INFO("--online wire: ioctl socket {:08X} cmd {:08X} arg {:08X}{}", socket_handle, cmd,
                 arg_ptr ? rex::byte_swap(*static_cast<const uint32_t*>(arg_ptr.host_address())) : 0u,
                 XFAILED(status) ? " (failed)" : "");
  }
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  // TODO
  return 0;
}

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  // With an online_address set, a socket bound to "any address" is bound to
  // that one instead: two copies on one machine, on 127.0.0.2 and 127.0.0.3,
  // can then both have the title's fixed ports.
  if (OnlineStub() && !REXCVAR_GET(online_address).empty() && native_name.sin_addr == 0u) {
    native_name.sin_addr = ntohl(OnlineLocalAddress());
  }
  if (OnlineStub()) {
    const uint32_t ip = native_name.sin_addr;
    REXKRNL_INFO("--online wire: bind socket {:08X} to {}.{}.{}.{}:{}", socket_handle, ip >> 24,
                 (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, uint16_t(native_name.sin_port));
  }
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  if (OnlineStub()) {
    const auto* in = reinterpret_cast<const XSOCKADDR_IN*>(name.host_address());
    const uint32_t ip = in->sin_addr;
    REXKRNL_INFO("--online wire: connect socket {:08X} to {}.{}.{}.{}:{}", socket_handle,
                 ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, uint16_t(in->sin_port));
  }
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
    SetLastSocketError();
    return -1;
  }

  return 0;
}

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
  if (OnlineStub()) {
    REXKRNL_INFO("--online wire: listen socket {:08X}", socket_handle);
  }
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> addr_ptr,
                        mapped_u32 addrlen_ptr) {
  if (!addr_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    return new_socket->handle();
  } else {
    return -1;
  }
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      if (!socket) {
        this->count = i;
        break;
      }
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

i32 NetDll_select_entry(i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  host_set host_readfds = {};
  fd_set native_readfds = {};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {};
  fd_set native_writefds = {};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {};
  fd_set native_exceptfds = {};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  // The title's nfds is WinSock's, which ignores it (titles pass 1 or 0). On
  // POSIX it bounds the descriptors examined - highest plus one - and with the
  // title's value no socket was ever looked at: a non-blocking connect never
  // showed as writable, so it never showed as complete.
  int native_nfds = nfds;
#if !REX_PLATFORM_WIN32
  native_nfds = 0;
  for (const host_set* set : {&host_readfds, &host_writefds, &host_exceptfds}) {
    for (uint32_t i = 0; i < set->count; ++i) {
      native_nfds = std::max(native_nfds, int(set->sockets[i]->native_handle()) + 1);
    }
  }
#endif
  int ret = select(native_nfds, readfds ? &native_readfds : nullptr,
                   writefds ? &native_writefds : nullptr, exceptfds ? &native_exceptfds : nullptr,
                   timeout_in);
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  // TODO(gibbed): modify ret to be what's actually copied to the guest fd_sets?
  return ret;
}

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  const int received = socket->Recv(buf_ptr, buf_len, flags);
  if (received < 0) {
    SetLastSocketError();
    return received;
  }
  LogWire("recv", socket_handle, static_cast<const uint8_t*>(buf_ptr.host_address()), received);
  return received;
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret =
      socket->RecvFrom(buf_ptr, buf_len, flags, &native_from, fromlen_ptr ? &native_fromlen : 0);

  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
SetLastSocketError();
  }

  return ret;
}

// The address at either end of a connected socket. DirtySock asks for the peer
// to learn that a non-blocking connect has completed; these were stubs.
u32 SocketName(u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, mapped_u32 name_len, bool peer) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    return -1;
  }
  sockaddr_in native = {};
#if REX_PLATFORM_WIN32
  const auto handle = SOCKET(socket->native_handle());
  int native_len = sizeof(native);
#else
  const int handle = int(socket->native_handle());
  socklen_t native_len = sizeof(native);
#endif
  const int ret =
      peer ? getpeername(handle, reinterpret_cast<sockaddr*>(&native), &native_len)
           : getsockname(handle, reinterpret_cast<sockaddr*>(&native), &native_len);
  if (ret < 0) {
    SetLastSocketError();
    return -1;
  }
  if (name) {
    name->sin_family = 2;  // AF_INET
    name->sin_port = ntohs(native.sin_port);
    name->sin_addr = ntohl(native.sin_addr.s_addr);
    std::memset(name->x_sin_zero, 0, sizeof(name->x_sin_zero));
  }
  if (name_len) {
    *name_len = sizeof(XSOCKADDR_IN);
  }
  return 0;
}

u32 NetDll_getpeername_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             mapped_u32 name_len) {
  return SocketName(socket_handle, name, name_len, true);
}

u32 NetDll_getsockname_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             mapped_u32 name_len) {
  return SocketName(socket_handle, name, name_len, false);
}

u32 NetDll_send_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  LogWire("send", socket_handle, static_cast<const uint8_t*>(buf_ptr.host_address()),
          int(buf_len));
  const int sent = socket->Send(buf_ptr, buf_len, flags);
  if (sent < 0) {
    SetLastSocketError();
  }
  return sent;
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags,
                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  const int sent = socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
  if (sent < 0) {
    SetLastSocketError();
  }
  return sent;
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
REX_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
REX_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
REX_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
REX_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
REX_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
REX_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
REX_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
REX_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
REX_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
REX_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
REX_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
REX_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
REX_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetConnect, rex::kernel::xam::NetDll_XNetConnect_entry)
REX_EXPORT(__imp__NetDll_XNetGetConnectStatus, rex::kernel::xam::NetDll_XNetGetConnectStatus_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
REX_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
REX_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
REX_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
REX_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
REX_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
REX_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
REX_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
REX_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
REX_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
REX_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
REX_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
REX_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
REX_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
REX_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
REX_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
REX_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
REX_EXPORT(__imp__NetDll_getpeername, rex::kernel::xam::NetDll_getpeername_entry)
REX_EXPORT(__imp__NetDll_getsockname, rex::kernel::xam::NetDll_getsockname_entry)
REX_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
REX_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
REX_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)

REX_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
REX_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
REX_EXPORT_STUB(__imp__NetDll_UpnpStartup);
REX_EXPORT_STUB(__imp__NetDll_WSACancelOverlappedIO);
REX_EXPORT_STUB(__imp__NetDll_WSAEventSelect);
REX_EXPORT_STUB(__imp__NetDll_WSAGetOverlappedResult);
REX_EXPORT_STUB(__imp__NetDll_WSARecv);
REX_EXPORT_STUB(__imp__NetDll_WSASend);
REX_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_XHttpConnect);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
REX_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpen);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpReadData);
REX_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
REX_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
REX_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
REX_EXPORT_STUB(__imp__NetDll_XHttpStartup);
REX_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
REX_EXPORT(__imp__NetDll_XNetCreateKey, rex::kernel::xam::NetDll_XNetCreateKey_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
REX_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
REX_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
REX_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
REX_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);
REX_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)
REX_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
REX_EXPORT(__imp__NetDll_XNetServerToInAddr, rex::kernel::xam::NetDll_XNetServerToInAddr_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
REX_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
REX_EXPORT(__imp__NetDll_XNetUnregisterInAddr, rex::kernel::xam::NetDll_XNetUnregisterInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterKey, rex::kernel::xam::NetDll_XNetUnregisterKey_entry)
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
REX_EXPORT_STUB(__imp__NetDll_XnpCapture);
REX_EXPORT_STUB(__imp__NetDll_XnpConfig);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
REX_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
REX_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
REX_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
REX_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
REX_EXPORT_STUB(__imp__NetDll_getsockopt);
