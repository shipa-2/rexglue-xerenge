/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <rex/kernel/xam/module.h>
#include <rex/logging/macros.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
// #include <rex/system/xnet.h>

#include <rex/net/socket.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

namespace rex::system {

// Shared UDP ports. The console lets several of the title's sockets have one
// port: a game of three needs it - each player keeps a socket on 8192 for
// every other player, and the one waiting for a newcomer is bound there while
// the one to the host already is. A host system refuses the second bind
// (EADDRINUSE), and the player already in the game could not take the third:
// the third stayed "connecting" until it gave up.
//
// So a socket whose bind failed on a port one of these sockets holds is noted,
// and if the title does not bind it elsewhere (as it does with the sockets it
// moves to port 0 or 6000) but receives or sends with it, it joins that port.
// The holder's native socket receives for all of them; each datagram goes to
// the member that has sent to its sender, one from an unknown sender to the
// newest member that has sent to no one yet - the one waiting for it - and
// every member sends from the shared port.
struct XSocket::PortGroup {
  struct Datagram {
    std::vector<uint8_t> data;
    sockaddr_in from;
  };
  struct Member {
    XSocket* socket;
    std::set<uint64_t> peers;  // address << 16 | port, as on the wire
    std::deque<Datagram> queue;
  };
  uint64_t key = 0;
  uint64_t fd = uint64_t(-1);
  std::vector<Member> members;  // the holder first, then by joining

  Member* Find(const XSocket* socket) {
    for (auto& member : members) {
      if (member.socket == socket) {
        return &member;
      }
    }
    return nullptr;
  }
};

namespace {

std::mutex g_port_groups_mutex;
std::map<uint64_t, std::weak_ptr<XSocket::PortGroup>> g_port_groups;

uint64_t PortKey(uint32_t address_be, uint16_t port_be) {
  return (uint64_t(ntohl(address_be)) << 16) | ntohs(port_be);
}

// NAT traversal (Xerenge #49). Players on the internet are behind routers
// that let a UDP packet in only from where one of their own went out to, and
// on a port of the router's choosing: the title, which sends to the address
// the lobby gave and to its fixed ports, reaches no one. So each of its UDP
// sockets registers with the lobby server on the directory port ("XNAT1 REG
// <port>", renewed every 20 s), the server noting where the router mapped it.
// A packet to another player's address first asks the server for that
// player's socket ("XNAT1 ASK"); the answer ("ANS") and the server's word to
// the other player ("MEET") make both sides send to each other's mapping, which
// opens both routers. From then on what the title sends there goes to the
// mapping, and what comes from the mapping comes to the title as from the
// address and port it knows. Off for a lobby on the player's own network.
namespace nat {

constexpr char kPrefix[] = "XNAT1 ";
constexpr uint16_t kServerPort = 31860;  // the lobby's directory port

std::mutex g_mutex;
uint32_t g_server = 0;  // the lobby server, as on the wire; 0: off
struct Mapping {
  uint32_t address = 0;  // as on the wire
  uint16_t port = 0;     // as on the wire
};
std::map<uint64_t, Mapping> g_out;      // by the address and port the title sends to
std::map<uint64_t, uint64_t> g_back;    // the mapping -> what the title knows it as
std::map<uint64_t, std::chrono::steady_clock::time_point> g_asked;
std::map<uint64_t, uint16_t> g_sockets;  // native socket -> its port (as on the wire)
bool g_keepalive = false;

bool IsLocal(uint32_t address_be) {
  const uint32_t a = ntohl(address_be);
  const uint32_t top = a >> 24, second = (a >> 16) & 0xFF;
  return top == 10 || top == 127 || top == 0 || top >= 224 || (top == 172 && second >= 16 && second < 32) ||
         (top == 192 && second == 168) || (top == 169 && second == 254) ||
         (top == 100 && second >= 64 && second < 128);
}

std::string Dotted(uint32_t address_be) {
  char text[INET_ADDRSTRLEN] = {};
  in_addr in = {};
  in.s_addr = address_be;
  inet_ntop(AF_INET, &in, text, sizeof(text));
  return text;
}

void SendText(uint64_t fd, const std::string& text, uint32_t address_be, uint16_t port_be) {
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = address_be;
  to.sin_port = port_be;
#if REX_PLATFORM_WIN32
  sendto(SOCKET(fd), text.data(), int(text.size()), 0, (sockaddr*)&to, sizeof(to));
#else
  sendto(int(fd), text.data(), text.size(), 0, (sockaddr*)&to, sizeof(to));
#endif
}

// Called with g_mutex held.
void Register(uint64_t fd, uint16_t port_be) {
  SendText(fd, std::string(kPrefix) + "REG " + std::to_string(ntohs(port_be)), g_server, htons(kServerPort));
}

void Add(uint64_t fd, uint16_t port_be) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_server || port_be == 0) {
    return;
  }
  g_sockets[fd] = port_be;
  Register(fd, port_be);
  if (!g_keepalive) {
    g_keepalive = true;
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [fd, port] : g_sockets) {
          Register(fd, port);
        }
      }
    }).detach();
  }
}

void Forget(uint64_t fd) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_sockets.erase(fd);
}

// Where a datagram to `to` goes (rewritten in place); asks the server once a
// second while the other player's mapping is not known.
void Outgoing(uint64_t fd, sockaddr_in* to) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_server || to->sin_addr.s_addr == g_server || IsLocal(to->sin_addr.s_addr)) {
    return;
  }
  const uint64_t key = PortKey(to->sin_addr.s_addr, to->sin_port);
  if (auto found = g_out.find(key); found != g_out.end()) {
    to->sin_addr.s_addr = found->second.address;
    to->sin_port = found->second.port;
    return;
  }
  const auto socket = g_sockets.find(fd);
  if (socket == g_sockets.end()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  auto& asked = g_asked[key];
  if (now - asked < std::chrono::seconds(1)) {
    return;
  }
  asked = now;
  SendText(fd,
           std::string(kPrefix) + "ASK " + std::to_string(ntohs(socket->second)) + " " +
               Dotted(to->sin_addr.s_addr) + " " + std::to_string(ntohs(to->sin_port)),
           g_server, htons(kServerPort));
}

// Players on this console's own network whose way to the internet is another
// (a VPN on one of the two): their address on the internet -> their address here.
std::map<uint32_t, uint32_t> g_neighbours;
// The mappings learnt from a player's own datagrams (Learn): the server's word
// for them (ANS, MEET), which may come after, no longer replaces them.
std::set<uint64_t> g_seen;

// Where a player really is for this socket, from a datagram of theirs. A
// player's router that gives each destination a port of its own (a VPN's exit,
// a carrier's NAT) sends from a port other than the one the server saw, and
// datagrams sent there were lost - the game session on 8192 never came up,
// though 6000 did, answered where it came from. A datagram from a player we
// know - from their address on the internet, or from their address here when
// they are on this network too - on this socket's port says where they are for
// it: sent there from now on (as ICE's peer-reflexive addresses). The title
// knows them by their address on the internet. Called with g_mutex held.
void Learn(uint64_t fd, const sockaddr_in& from) {
  if (g_back.count(PortKey(from.sin_addr.s_addr, from.sin_port))) {
    return;
  }
  const auto socket = g_sockets.find(fd);
  if (socket == g_sockets.end()) {
    return;
  }
  uint32_t player = 0;
  for (const auto& [on_internet, here] : g_neighbours) {
    if (here == from.sin_addr.s_addr) {
      player = on_internet;
    }
  }
  if (!player && !IsLocal(from.sin_addr.s_addr)) {
    for (const auto& [key, mapping] : g_out) {
      if (mapping.address == from.sin_addr.s_addr) {
        player = from.sin_addr.s_addr;
      }
    }
  }
  if (!player) {
    return;
  }
  const uint64_t key = PortKey(player, socket->second);
  const Mapping seen{from.sin_addr.s_addr, from.sin_port};
  if (auto out = g_out.find(key); out == g_out.end() || out->second.port != seen.port ||
                                  out->second.address != seen.address) {
    REXKRNL_INFO("--online: NAT - {}:{} is reached at {}:{} (seen)", Dotted(player), ntohs(socket->second),
                 Dotted(seen.address), ntohs(seen.port));
    g_out[key] = seen;
  }
  g_seen.insert(key);
  g_back[PortKey(seen.address, seen.port)] = key;
}

// True for this traversal's own datagrams, which the title never sees; a
// datagram from a mapping comes as from the address the title knows.
bool Incoming(uint64_t fd, const uint8_t* data, size_t length, sockaddr_in* from) {
  const size_t prefix = sizeof(kPrefix) - 1;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_server) {
    return false;
  }
  if (length > prefix && std::memcmp(data, kPrefix, prefix) == 0) {
    char text[128] = {};
    std::memcpy(text, data + prefix, std::min(length - prefix, sizeof(text) - 1));
    char verb[8] = {}, known[INET_ADDRSTRLEN] = {}, mapped[INET_ADDRSTRLEN] = {};
    unsigned known_port = 0, mapped_port = 0;
    in_addr known_address = {}, mapped_address = {};
    if (from->sin_addr.s_addr == g_server &&
        std::sscanf(text, "%7s %15s %u %15s %u", verb, known, &known_port, mapped, &mapped_port) == 5 &&
        (!std::strcmp(verb, "ANS") || !std::strcmp(verb, "MEET")) && known_port && known_port <= 0xFFFF &&
        mapped_port && mapped_port <= 0xFFFF && inet_pton(AF_INET, known, &known_address) == 1 &&
        inet_pton(AF_INET, mapped, &mapped_address) == 1) {
      const uint64_t key = PortKey(known_address.s_addr, htons(uint16_t(known_port)));
      const Mapping mapping{mapped_address.s_addr, htons(uint16_t(mapped_port))};
      if (g_out.find(key) == g_out.end()) {
        REXKRNL_INFO("--online: NAT - {}:{} is reached at {}:{} ({})", known, known_port, mapped, mapped_port,
                     verb);
      }
      if (!g_seen.count(key)) {
        g_out[key] = mapping;
        g_back[PortKey(mapping.address, mapping.port)] = key;
      }
      for (int i = 0; i < 2; ++i) {
        SendText(fd, std::string(kPrefix) + "HI", mapping.address, mapping.port);
        // ... and to the player's address here, when on this network too.
        if (auto here = g_neighbours.find(known_address.s_addr); here != g_neighbours.end()) {
          SendText(fd, std::string(kPrefix) + "HI", here->second, htons(uint16_t(known_port)));
        }
      }
    }
    // A player's HI: where they are for this socket (the game's own first
    // datagram may not come before ours have reached them).
    if (from->sin_addr.s_addr != g_server) {
      Learn(fd, *from);
    }
    return true;
  }
  Learn(fd, *from);
  auto found = g_back.find(PortKey(from->sin_addr.s_addr, from->sin_port));
  if (found != g_back.end()) {
    from->sin_addr.s_addr = htonl(uint32_t(found->second >> 16));
    from->sin_port = htons(uint16_t(found->second & 0xFFFF));
  }
  return false;
}

// A copy of a datagram to a player on this network too, at their address here,
// until one of theirs has come from there (and the title's sends go there by
// themselves). `to` as the title gave it, before Outgoing.
bool NeighbourCopy(const sockaddr_in& to, sockaddr_in* copy) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto here = g_neighbours.find(to.sin_addr.s_addr);
  if (!g_server || here == g_neighbours.end()) {
    return false;
  }
  if (auto out = g_out.find(PortKey(to.sin_addr.s_addr, to.sin_port));
      out != g_out.end() && out->second.address == here->second) {
    return false;
  }
  *copy = to;
  copy->sin_addr.s_addr = here->second;
  return true;
}

}  // namespace nat

}  // namespace

void XSocket::SetNatNeighbour(uint32_t public_be, uint32_t local_be) {
  std::lock_guard<std::mutex> lock(nat::g_mutex);
  nat::g_neighbours[public_be] = local_be;
}

void XSocket::SetNatServer(uint32_t address_be) {
  std::lock_guard<std::mutex> lock(nat::g_mutex);
  nat::g_server = nat::IsLocal(address_be) ? 0 : address_be;
}

std::shared_ptr<XSocket::PortGroup> XSocket::Shared() {
  if (group_ || share_key_ == 0 || bound_) {
    return group_;
  }
  std::lock_guard<std::mutex> lock(g_port_groups_mutex);
  auto found = g_port_groups.find(share_key_);
  std::shared_ptr<PortGroup> group = found != g_port_groups.end() ? found->second.lock() : nullptr;
  share_key_ = 0;
  if (!group) {
    return nullptr;
  }
  group->members.push_back(PortGroup::Member{this, {}, {}});
  group_ = group;
  REXKRNL_INFO("--online wire: a socket joins port {} shared by {} socket(s)",
               uint16_t(group->key & 0xFFFF), group->members.size());
  return group_;
}

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  share_key_ = 0;
  if (auto group = std::move(group_)) {
    std::lock_guard<std::mutex> lock(g_port_groups_mutex);
    auto& members = group->members;
    members.erase(std::remove_if(members.begin(), members.end(),
                                 [&](const PortGroup::Member& m) { return m.socket == this; }),
                  members.end());
    if (native_handle_ == group->fd) {
      if (!members.empty()) {
        // The port stays with those still sharing it: its native socket
        // goes when the last of them does.
        native_handle_ = uint64_t(-1);
        return X_STATUS_SUCCESS;
      }
      g_port_groups.erase(group->key);
    } else if (members.empty() && group->fd != uint64_t(-1)) {
      g_port_groups.erase(group->key);
      nat::Forget(group->fd);
      rex::net::socket_close(group->fd);
    }
  }
  if (native_handle_ == uint64_t(-1)) {
    return X_STATUS_SUCCESS;
  }

  nat::Forget(native_handle_);
  int ret = rex::net::socket_close(native_handle_);
  native_handle_ = -1;
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  int ret = setsockopt(native_handle_, level, optname, (char*)optval_ptr, optlen);
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  // The title's codes are WinSock's and its argument a big-endian u_long. On
  // POSIX the codes differ (FIONBIO 0x5421), so the raw call failed: a socket
  // the title made non-blocking stayed blocking, and the lobby's first recv
  // with nothing to read froze the title until the peer went away.
  constexpr uint32_t kWsaFionbio = 0x8004667E;
  constexpr uint32_t kWsaFionread = 0x4004667F;
  if (cmd == kWsaFionbio && arg_ptr) {
    uint32_t value;
    std::memcpy(&value, arg_ptr, sizeof(value));
#if REX_PLATFORM_WIN32
    u_long enable = value ? 1 : 0;
    const int ret = ioctlsocket(SOCKET(native_handle_), FIONBIO, &enable);
#else
    const int flags = fcntl(int(native_handle_), F_GETFL, 0);
    const int ret = flags < 0 ? -1
                              : fcntl(int(native_handle_), F_SETFL,
                                      value ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
    return ret < 0 ? X_STATUS_UNSUCCESSFUL : X_STATUS_SUCCESS;
  }
  if (cmd == kWsaFionread && arg_ptr) {
#if REX_PLATFORM_WIN32
    u_long available = 0;
    const int ret = ioctlsocket(SOCKET(native_handle_), FIONREAD, &available);
#else
    int available = 0;
    const int ret = ioctl(int(native_handle_), FIONREAD, &available);
#endif
    if (ret < 0) {
      return X_STATUS_UNSUCCESSFUL;
    }
    const uint32_t value = rex::byte_swap(uint32_t(available));
    std::memcpy(arg_ptr, &value, sizeof(value));
    return X_STATUS_SUCCESS;
  }
  int ret = rex::net::socket_ioctl(native_handle_, cmd, arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  int ret = connect(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  // Ports below 1024 are privileged on Linux; the title uses 1000. Every build maps them the same way so peers still agree.
  N_XSOCKADDR_IN mapped = *name;
  mapped.sin_port = MapWirePort(uint16_t(name->sin_port));
  int ret = bind(native_handle_, (sockaddr*)&mapped, name_len);
  sockaddr_in native = {};
  std::memcpy(&native, &mapped, std::min(sizeof(native), sizeof(mapped)));
  const uint64_t key = PortKey(native.sin_addr.s_addr, native.sin_port);
  const bool shareable = type_ == X_SOCK_DGRAM && native.sin_port != 0;
  if (ret < 0) {
    // Held by another of these sockets: this one shares it, as on the console
    // (the title binds a socket per player to 8192 and gives up on one that
    // fails - CommUDP connect ":8192:8192#..." answers -5).
    if (shareable) {
      std::lock_guard<std::mutex> lock(g_port_groups_mutex);
      auto found = g_port_groups.find(key);
      if (auto group = found != g_port_groups.end() ? found->second.lock() : nullptr) {
        group->members.push_back(PortGroup::Member{this, {}, {}});
        group_ = group;
        bound_ = true;
        bound_port_ = name->sin_port;
        share_key_ = 0;
        REXKRNL_INFO("--online wire: port {} now shared by {} socket(s)",
                     uint16_t(name->sin_port), group->members.size());
        return X_STATUS_SUCCESS;
      }
    }
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = name->sin_port;
  share_key_ = 0;
  if (shareable) {
    auto group = std::make_shared<PortGroup>();
    group->key = key;
    group->fd = native_handle_;
    group->members.push_back(PortGroup::Member{this, {}, {}});
    std::lock_guard<std::mutex> lock(g_port_groups_mutex);
    g_port_groups[key] = group;
    group_ = group;
  }
  if (type_ == X_SOCK_DGRAM) {
    sockaddr_in got = {};
    socklen_t got_length = sizeof(got);
    if (getsockname(native_handle_, (sockaddr*)&got, &got_length) == 0) {
      nat::Add(native_handle_, got.sin_port);
    }
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr n_sockaddr;
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(native_handle_, &n_sockaddr, &n_name_len);
  if (ret == -1) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  return shutdown(native_handle_, how);
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

  sockaddr_in nfrom;
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret;
  auto group = Shared();
  if (group && group->members.size() > 1) {
    // A shared port: take what its native socket has, hand each datagram to
    // its member, then this member's first.
    std::lock_guard<std::mutex> lock(g_port_groups_mutex);
    for (;;) {
      PortGroup::Datagram datagram;
      datagram.data.resize(65536);
      socklen_t length = sizeof(datagram.from);
#if REX_PLATFORM_WIN32
      u_long available = 0;
      if (ioctlsocket(SOCKET(group->fd), FIONREAD, &available) != 0 || available == 0) {
        break;
      }
      const int got = recvfrom(SOCKET(group->fd), reinterpret_cast<char*>(datagram.data.data()),
                               int(datagram.data.size()), 0, (sockaddr*)&datagram.from, &length);
#else
      const int got = int(recvfrom(int(group->fd), datagram.data.data(), datagram.data.size(),
                                   MSG_DONTWAIT, (sockaddr*)&datagram.from, &length));
#endif
      if (got < 0) {
        break;
      }
      datagram.data.resize(size_t(got));
      if (nat::Incoming(group->fd, datagram.data.data(), datagram.data.size(), &datagram.from)) {
        continue;
      }
      const uint64_t sender = PortKey(datagram.from.sin_addr.s_addr, datagram.from.sin_port);
      PortGroup::Member* to = nullptr;
      for (auto& member : group->members) {
        if (member.peers.count(sender)) {
          to = &member;
          break;
        }
      }
      for (auto it = group->members.rbegin(); !to && it != group->members.rend(); ++it) {
        if (it->peers.empty()) {
          to = &*it;
        }
      }
      if (!to) {
        to = &group->members.front();
      }
      to->peers.insert(sender);
      to->queue.push_back(std::move(datagram));
    }
    PortGroup::Member* self = group->Find(this);
    if (!self || self->queue.empty()) {
#if REX_PLATFORM_WIN32
      WSASetLastError(WSAEWOULDBLOCK);
#else
      errno = EAGAIN;
#endif
      return -1;
    }
    PortGroup::Datagram datagram = std::move(self->queue.front());
    self->queue.pop_front();
    ret = int(std::min<size_t>(datagram.data.size(), buf_len));
    std::memcpy(buf, datagram.data.data(), size_t(ret));
    nfrom = datagram.from;
  } else {
    do {
      nfromlen = sizeof(sockaddr_in);
      ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags, (sockaddr*)&nfrom, &nfromlen);
    } while (ret > 0 && nat::Incoming(native_handle_, buf, size_t(ret), &nfrom));
  }
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);  // BE <- BE
    from->sin_port = UnmapWirePort(ntohs(nfrom.sin_port));
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  sockaddr_in nto = {};
  if (to) {
    // sin_addr and sin_port are already big-endian in memory, as the native struct wants.
    std::memcpy(&nto.sin_addr, &to->sin_addr, sizeof(nto.sin_addr));
    const rex::be<uint16_t> wire_port = MapWirePort(uint16_t(to->sin_port));
    std::memcpy(&nto.sin_port, &wire_port, sizeof(nto.sin_port));
    nto.sin_family = AF_INET;
  }

  uint64_t fd = native_handle_;
  if (auto group = Shared()) {
    // Sent from the shared port; replies from there come back to this member.
    std::lock_guard<std::mutex> lock(g_port_groups_mutex);
    fd = group->fd;
    if (to) {
      if (PortGroup::Member* self = group->Find(this)) {
        self->peers.insert(PortKey(nto.sin_addr.s_addr, nto.sin_port));
      }
    }
  }
  sockaddr_in here = {};
  const bool to_here_too = to && nat::NeighbourCopy(nto, &here);
  if (to) {
    nat::Outgoing(fd, &nto);
  }
  const int sent = sendto(fd, reinterpret_cast<char*>(buf), buf_len, flags,
                          to ? (sockaddr*)&nto : nullptr, to_len);
  if (to_here_too) {
    sendto(fd, reinterpret_cast<char*>(buf), buf_len, flags, (sockaddr*)&here, sizeof(here));
  }
  return sent;
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
