// System link on host Winsock. The game reaches the network only through the XNet winsock thunks, replaced
// here. The title's XNet object stays null, so the XNet calls not replaced (Live QoS and the like) fail with
// WSANOTINITIALISED on their own, and the XOnline calls with XONLINE_E_NOT_INITIALIZED.
// Ports: as on the console, all of system link travels on one UDP port (3074, or the system_link_port setting), and
// the title's UDP ports (Conker binds 1000 and 1001, which a Linux user can't bind) are link ports inside it: each
// datagram carries its sender's and receiver's (Socket, LinkPump). TCP stays on host sockets.
// Addresses: as on the console, the IN_ADDR the title sees for a peer is opaque (10.254.x.y, one per peer), and each
// stands for the peer's real IPv4 and system link port, so two copies on one PC, or behind one NAT, are two peers.
// An XNADDR carries its owner's address and port, and its Ethernet address the two mixed, so copies sharing an IP
// still differ.
// With --instance N the game binds 127.0.0.N and a broadcast goes to the other loopback instances; with --port too,
// it is a copy like any other (run-linux.sh --instances: the first hosts, the others join it on 127.0.0.1).
// Join address (host[:port], a host past the LAN or another copy on this PC): the system link search's broadcasts
// also go to it, and every broadcast goes to the peers a broadcast can't reach (past the LAN, or on another port).
// A LAN address in an XNADDR only means something on its owner's LAN: from past it, it stands for the join address
// on the client, and on the host for the peer whose packets come from past the LAN.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include "xdk/winsock_posix.h"
#endif

#include <algorithm>
#include <array>
#include <random>

#include "app/app.h"
#include "xdk/hle.h"

using kernel::HostPtr;
using kernel::R16;
using kernel::R32;
using kernel::W32;

namespace {

// XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20]
constexpr uint32_t kXnAddrSize = 0x24;
constexpr uint32_t kXnAddrPort = 0x08;  // wPortOnline: the owner's system link port, network order
constexpr uint32_t kXnAddrEnet = 0x0A;
// WSAOVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent
constexpr uint32_t kOvlStatus = 0x00;
constexpr uint32_t kOvlBytes = 0x04;
constexpr uint32_t kOvlEvent = 0x10;
constexpr uint32_t kOvlPending = 0x103;
// XNDNS: iStatus, cina, aina[8]
constexpr uint32_t kXnDnsSize = 0x28;
// fd_set: fd_count, fd_array[64]
constexpr uint32_t kFdSetMax = 64;

constexpr uint32_t kSocketError = 0xFFFFFFFF;
constexpr uint32_t kLinkUp = 0x01 | 0x02 | 0x08;          // active, 100 Mbps, full duplex
constexpr uint32_t kXnAddrEthernet = 0x02 | 0x04;         // ethernet, static address
constexpr DWORD kUdpConnReset = 0x9800000C;  // SIO_UDP_CONNRESET: off, or a send to a closed port fails the next recv
constexpr int kLoopbackInstances = 8;
constexpr uint16_t kSystemLinkPort = 3074;
constexpr uint32_t kPeerNet = 0x0AFE0000;  // 10.254.0.0/16, host order: the title's addresses for peers
constexpr size_t kMaxPeers = 0xFFFE;

// a title's UDP socket is a port of the link (vport), as on the console, where every system link port travels inside
// UDP 3074: its host socket is on the loopback, and LinkPump hands it what comes for its port
struct Socket {
    SOCKET s = INVALID_SOCKET;
    bool udp = false;
    bool nonblocking = false;  // the guest's FIONBIO; the host socket is always nonblocking
    uint16_t vport = 0;        // UDP: the link port bound, network order, 0 for none yet
    sockaddr_in inbox{};       // UDP: where its host socket is, which LinkPump sends to
    sockaddr_in to{};          // UDP: the address connect gave (the title's), sin_family 0 for none
};

// on the wire: kMagic, then the sender's and the receiver's link ports
constexpr uint16_t kMagic = 0x4B43;  // "CK"
constexpr int kWireHeader = 6;
// into a socket's inbox: the sender's address, then its link port
constexpr int kInboxHeader = 8;
constexpr uint16_t kFirstEphemeral = 49152;

struct Span {
    uint32_t ptr, len;
};

// the byte count and flags pointers are only written when the call completes at once, as in Winsock:
// a title may pass its flags on the stack
struct PendingRecv {
    uint32_t sock, ovl, from, fromlen;
    std::vector<Span> spans;  // copied: the title's WSABUF array may live on its stack
};

std::mutex g_lock;
std::unordered_map<uint32_t, Socket> g_sockets;
uint32_t g_next_socket = 0x100;
std::vector<PendingRecv> g_pending;

std::once_flag g_init;
SOCKET g_link = INVALID_SOCKET;  // the system link socket, on g_port
std::atomic<int> g_link_error{0};  // why it couldn't bind g_port, 0 when it did
uint16_t g_next_vport = kFirstEphemeral;  // host order, under g_lock
std::atomic<bool> g_ready{false};  // Init has run: g_local and g_port are set
int g_instance = 0;
uint32_t g_local = 0;  // network order
uint16_t g_port = kSystemLinkPort;  // what the system link port binds, host order (read at Init)
std::array<uint8_t, 8> g_kid{};

thread_local uint32_t t_error = 0;

struct Endpoint {
    uint32_t ip = 0;    // network order; this PC's loopback is g_local
    uint16_t port = 0;  // network order
    bool operator==(const Endpoint&) const = default;
};

// the peers seen, by their real address; the title's address for g_peers[i] is Opaque(i). Under g_lock if both.
struct Peer {
    Endpoint at;
    std::array<uint8_t, kXnAddrSize> xnaddr{};
    bool has_xnaddr = false;
};
std::mutex g_peer_lock;
std::vector<Peer> g_peers;
int g_last_remote = -1;  // the last peer past the LAN a packet came from

std::string Ip(uint32_t a) {
    char s[32];
    std::snprintf(s, sizeof(s), "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
    return s;
}
std::string Addr(const sockaddr_in& a) { return Ip(a.sin_addr.s_addr) + ":" + std::to_string(ntohs(a.sin_port)); }

bool Private(uint32_t a) {
    uint8_t b0 = a & 0xFF, b1 = (a >> 8) & 0xFF;
    return b0 == 10 || b0 == 127 || (b0 == 172 && (b1 & 0xF0) == 16) || (b0 == 192 && b1 == 168) ||
           (b0 == 169 && b1 == 254);
}

uint32_t Opaque(size_t peer) { return htonl(kPeerNet | uint32_t(peer + 1)); }
int PeerOf(uint32_t opaque) {
    uint32_t a = ntohl(opaque);
    if ((a & 0xFFFF0000) != kPeerNet || !(a & 0xFFFF)) return -1;
    return int(a & 0xFFFF) - 1;
}

// this PC is one address: copies on it reach each other on its loopback, and their XNADDRs hold its LAN address
uint32_t Fold(uint32_t ip) { return !g_instance && (ntohl(ip) >> 24) == 127 ? g_local : ip; }

// with g_peer_lock held
int FindPeer(const Endpoint& at) {
    for (size_t i = 0; i < g_peers.size(); i++)
        if (g_peers[i].at == at) return int(i);
    return -1;
}
int AddPeer(const Endpoint& at) {
    if (int i = FindPeer(at); i >= 0) return i;
    if (g_peers.size() >= kMaxPeers) return -1;
    g_peers.push_back({at});
    LOGR("xnet: peer %s:%u is %s", Ip(at.ip).c_str(), ntohs(at.port), Ip(Opaque(g_peers.size() - 1)).c_str());
    return int(g_peers.size() - 1);
}

// the join address (host, or host:port with the port its system link is on), resolved when it changes; ip 0 for none
Endpoint JoinAddress() {
    static std::mutex m;
    static std::string last;
    static Endpoint address;
    std::string text = app::JoinArg();
    if (text.empty()) {
        std::lock_guard<std::recursive_mutex> ui(app::UiLock());
        text = app::Get().join_ip;
    }
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);
    std::lock_guard<std::mutex> lock(m);
    if (text == last) return address;
    last = text;
    address = {};
    std::string host = text;
    uint16_t port = kSystemLinkPort;
    if (size_t colon = text.rfind(':'); colon != std::string::npos && text.find(':') == colon) {
        host = text.substr(0, colon);
        port = uint16_t(std::strtoul(text.c_str() + colon + 1, nullptr, 10));
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (!host.empty() && port && getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0) {
        address = {Fold(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr), htons(port)};
        freeaddrinfo(res);
    }
    if (!text.empty())
        LOGR("xnet: join %s -> %s", text.c_str(),
             address.ip ? (Ip(address.ip) + ":" + std::to_string(port)).c_str() : "not found");
    return address;
}

uint32_t PrimaryAddress() {
    uint32_t addr = htonl(INADDR_LOOPBACK);
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808);  // no packet is sent: connect only picks the route
    sockaddr_in me{};
    int len = sizeof(me);
    if (s != INVALID_SOCKET && ::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0 &&
        ::getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0 && me.sin_addr.s_addr)
        addr = me.sin_addr.s_addr;
    if (s != INVALID_SOCKET) ::closesocket(s);
    return addr;
}

void Pump();
void LinkPump();

void Init() {
    std::call_once(g_init, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        g_instance = app::PortArg() ? 0 : app::Instance();
        g_local = g_instance ? htonl(0x7F000000u | uint32_t(g_instance)) : PrimaryAddress();
        {
            std::lock_guard<std::recursive_mutex> ui(app::UiLock());
            uint32_t p = app::PortArg() ? uint32_t(app::PortArg()) : app::Get().system_link_port;
            if (p && p <= 0xFFFF) g_port = uint16_t(p);
        }
        LOGR("xnet: %s address %s, system link on UDP %u", g_instance ? "loopback instance" : "LAN",
             Ip(g_local).c_str(), g_port);
        g_link = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BOOL on = 1, off = FALSE;
        DWORD ret = 0;
        ::setsockopt(g_link, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
        WSAIoctl(g_link, kUdpConnReset, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = g_instance ? g_local : INADDR_ANY;
        at.sin_port = htons(g_port);
        if (::bind(g_link, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0) {
            int e = WSAGetLastError();
            g_link_error = e;
            LOGR("xnet: system link can't bind %s (%d)%s", Addr(at).c_str(), e,
                 e == WSAEADDRINUSE ? ": the port is in use, by another copy? Give each its own system link port" : "");
        }
        g_ready = true;
        std::thread(Pump).detach();
        std::thread(LinkPump).detach();
    });
}

uint32_t Fail(int err) {
    t_error = uint32_t(err);
    return kSocketError;
}
int HostError() {
    int e = WSAGetLastError();
    return e == WSAECONNRESET ? WSAEWOULDBLOCK : e;  // a port unreachable reply from an earlier send
}

Socket Find(uint32_t sock) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_sockets.find(sock);
    return it == g_sockets.end() ? Socket{} : it->second;
}

sockaddr_in GuestAddr(uint32_t va) {
    sockaddr_in a{};
    std::memcpy(&a, HostPtr(va), sizeof(a));
    return a;
}

void Signal(uint32_t event) {
    if (!event) return;
    if (auto o = kernel::FromHandle(event)) kernel::SetEvent(*o);
}

bool Wait(SOCKET s, bool write, int ms) {
    fd_set f;
    FD_ZERO(&f);
    FD_SET(s, &f);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return ::select(0, write ? nullptr : &f, write ? &f : nullptr, nullptr, &tv) > 0;
}

std::vector<Span> Spans(uint32_t bufs, uint32_t count) {
    std::vector<Span> v;
    for (uint32_t i = 0; i < count; i++) v.push_back({R32(bufs + 8 * i + 4), R32(bufs + 8 * i)});
    return v;
}

// a datagram's source as the title sees it: a peer at its opaque address, itself at its own; the port is the link
// port it was sent from
sockaddr_in ToGuest(const Endpoint& at, uint16_t vport) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = vport;
    a.sin_addr.s_addr = g_local;
    if (at.ip == g_local && at.port == htons(g_port)) return a;
    std::lock_guard<std::mutex> lock(g_peer_lock);
    int i = AddPeer(at);
    if (i < 0) return a;
    if (!Private(at.ip)) g_last_remote = i;
    a.sin_addr.s_addr = Opaque(size_t(i));
    return a;
}

// the link endpoint of an address the title sends to: a peer's, this copy's own, or another copy's system link port
Endpoint ToReal(uint32_t ip) {
    if (int i = PeerOf(ip); i >= 0) {
        std::lock_guard<std::mutex> lock(g_peer_lock);
        if (size_t(i) < g_peers.size()) return g_peers[size_t(i)].at;
    }
    if (ip == g_local) return {g_local, htons(g_port)};
    return {Fold(ip), htons(kSystemLinkPort)};
}

// what came on the link for link port vport into the inbox of the socket bound to it; dropped when none is
void Deliver(const Endpoint& from, uint16_t from_vport, uint16_t vport, const uint8_t* data, int len) {
    sockaddr_in inbox{};
    {
        std::lock_guard<std::mutex> lock(g_lock);
        for (auto& [id, k] : g_sockets)
            if (k.udp && k.vport == vport) inbox = k.inbox;
    }
    if (!inbox.sin_family) {
        LOG_FIRST(20, "xnet: %d bytes for port %u, which nothing is bound to", len, ntohs(vport));
        return;
    }
    thread_local std::vector<uint8_t> buf(65536 + kInboxHeader);
    std::memcpy(buf.data(), &from.ip, 4);
    std::memcpy(buf.data() + 4, &from.port, 2);
    std::memcpy(buf.data() + 6, &from_vport, 2);
    std::memcpy(buf.data() + kInboxHeader, data, size_t(len));
    ::sendto(g_link, reinterpret_cast<const char*>(buf.data()), len + kInboxHeader, 0,
             reinterpret_cast<const sockaddr*>(&inbox), sizeof(inbox));
}

void LinkPump() {
    std::vector<uint8_t> buf(65536);
    for (;;) {
        if (!Wait(g_link, false, 100)) continue;
        sockaddr_in src{};
        int sl = sizeof(src);
        int n = ::recvfrom(g_link, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                           reinterpret_cast<sockaddr*>(&src), &sl);
        if (n < 0) {
            if (int e = HostError(); e != WSAEWOULDBLOCK) {
                LOG_FIRST(20, "xnet: link recv failed (%d)", e);
                Sleep(10);
            }
            continue;
        }
        uint16_t magic, from_vport, vport;
        std::memcpy(&magic, buf.data(), 2);
        if (n < kWireHeader || magic != htons(kMagic)) continue;  // not one of ours, or an inbox's own header
        std::memcpy(&from_vport, buf.data() + 2, 2);
        std::memcpy(&vport, buf.data() + 4, 2);
        Endpoint from{Fold(src.sin_addr.s_addr), src.sin_port};
        if (from.ip == g_local && from.port == htons(g_port)) continue;  // its own broadcast
        Deliver(from, from_vport, vport, buf.data() + kWireHeader, n - kWireHeader);
    }
}

// one datagram into guest buffers: bytes, or -1 with err
int Recv(uint32_t sock, const Socket& k, const std::vector<Span>& spans, uint32_t from, uint32_t fromlen, int& err) {
    thread_local std::vector<uint8_t> buf(65536 + kInboxHeader);
    sockaddr_in src{};
    int sl = sizeof(src);
    int n = ::recvfrom(k.s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&src), &sl);
    if (n < 0) {
        err = HostError();
        if (err != WSAEWOULDBLOCK) LOG_FIRST(20, "xnet: recv %X failed (%d)", sock, err);
        return -1;
    }
    const uint8_t* data = buf.data();
    if (k.udp) {
        if (n < kInboxHeader) {
            err = WSAEWOULDBLOCK;
            return -1;
        }
        Endpoint at;
        uint16_t vport;
        std::memcpy(&at.ip, data, 4);
        std::memcpy(&at.port, data + 4, 2);
        std::memcpy(&vport, data + 6, 2);
        src = ToGuest(at, vport);
        data += kInboxHeader;
        n -= kInboxHeader;
    }
    int done = 0;
    for (const Span& sp : spans) {
        int c = std::min<int>(n - done, int(sp.len));
        if (c <= 0) break;
        std::memcpy(HostPtr(sp.ptr), data + done, size_t(c));
        done += c;
    }
    if (done < n) LOG_FIRST(10, "xnet: %d byte datagram cut to %d", n, done);
    if (from) std::memcpy(HostPtr(from), &src, sizeof(src));
    if (fromlen) W32(fromlen, sizeof(src));
    return done;
}

// a UDP socket sending before it is bound gets an ephemeral link port, as Winsock binds one
uint16_t AutoBind(uint32_t sock);

int SendOnce(uint32_t sock, const Socket& k, const uint8_t* data, int len, const sockaddr_in* to) {
    if (!k.udp) {
        if (!to) return ::send(k.s, reinterpret_cast<const char*>(data), len, 0);
        sockaddr_in d = *to;
        d.sin_addr.s_addr = ToReal(d.sin_addr.s_addr).ip;
        return ::sendto(k.s, reinterpret_cast<const char*>(data), len, 0, reinterpret_cast<const sockaddr*>(&d), sizeof(d));
    }
    const sockaddr_in& dest = to ? *to : k.to;
    uint16_t from_vport = k.vport ? k.vport : AutoBind(sock);
    thread_local std::vector<uint8_t> buf(65536 + kWireHeader);
    len = std::min(len, 65536);
    const uint16_t magic = htons(kMagic);
    std::memcpy(buf.data(), &magic, 2);
    std::memcpy(buf.data() + 2, &from_vport, 2);
    std::memcpy(buf.data() + 4, &dest.sin_port, 2);
    std::memcpy(buf.data() + kWireHeader, data, size_t(len));
    auto send = [&](const Endpoint& e) {
        if (e.ip == g_local && e.port == htons(g_port)) {  // to itself, past the wire
            Deliver(e, from_vport, dest.sin_port, data, len);
            return len;
        }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = e.ip;
        a.sin_port = e.port;
        int n = ::sendto(g_link, reinterpret_cast<const char*>(buf.data()), len + kWireHeader, 0,
                         reinterpret_cast<const sockaddr*>(&a), sizeof(a));
        return n < 0 ? n : len;
    };
    if (dest.sin_addr.s_addr != INADDR_BROADCAST) return send(ToReal(dest.sin_addr.s_addr));
    // a broadcast reaches the LAN's copies on the system link port; the join address and the peers it can't reach
    // (past the LAN, or on another port) are sent it one by one
    std::vector<Endpoint> also;
    if (Endpoint join = JoinAddress(); join.ip && !(join.ip == g_local && join.port == htons(g_port)))
        also.push_back(join);
    {
        std::lock_guard<std::mutex> lock(g_peer_lock);
        for (const Peer& p : g_peers)
            if ((!Private(p.at.ip) || p.at.port != htons(kSystemLinkPort)) &&
                std::find(also.begin(), also.end(), p.at) == also.end())
                also.push_back(p.at);
    }
    for (const Endpoint& e : also) send(e);
    if (g_instance) {
        for (int i = 1; i <= kLoopbackInstances; i++)
            if (i != g_instance) send({htonl(0x7F000000u | uint32_t(i)), htons(g_port)});
        return len;
    }
    return send({INADDR_BROADCAST, htons(kSystemLinkPort)});
}

// bytes sent, or -1 with err
int Send(uint32_t sock, const Socket& k, const uint8_t* data, int len, const sockaddr_in* to, int& err) {
    if (k.udp && !to && !k.to.sin_family) {
        err = WSAENOTCONN;
        return -1;
    }
    int n = SendOnce(sock, k, data, len, to);
    if (n < 0) {
        err = HostError();
        LOG_FIRST(20, "xnet: send %X failed (%d)", sock, err);
    }
    return n;
}

std::vector<uint8_t> Gather(uint32_t bufs, uint32_t count) {
    std::vector<uint8_t> v;
    for (const Span& sp : Spans(bufs, count)) {
        auto p = static_cast<const uint8_t*>(HostPtr(sp.ptr));
        v.insert(v.end(), p, p + sp.len);
    }
    return v;
}

void Complete(uint32_t ovl, uint32_t status, uint32_t bytes) {
    W32(ovl + kOvlBytes, bytes);
    W32(ovl + kOvlStatus, status);
    Signal(R32(ovl + kOvlEvent));
}

// with g_lock held: false while no datagram is waiting
bool TryComplete(PendingRecv& p) {
    auto it = g_sockets.find(p.sock);
    if (it == g_sockets.end()) {
        Complete(p.ovl, WSAENOTSOCK, 0);
        return true;
    }
    int err = 0;
    int n = Recv(p.sock, it->second, p.spans, p.from, p.fromlen, err);
    if (n < 0 && err == WSAEWOULDBLOCK) return false;
    Complete(p.ovl, n < 0 ? uint32_t(err) : 0, n < 0 ? 0 : uint32_t(n));
    return true;
}

void Pump() {
    for (;;) {
        fd_set set;
        FD_ZERO(&set);
        {
            std::lock_guard<std::mutex> lock(g_lock);
            for (auto& p : g_pending) {
                auto it = g_sockets.find(p.sock);
                if (it != g_sockets.end() && set.fd_count < FD_SETSIZE) FD_SET(it->second.s, &set);
            }
        }
        if (!set.fd_count) {
            Sleep(1);
            continue;
        }
        timeval tv{0, 2000};
        if (::select(0, &set, nullptr, nullptr, &tv) <= 0) {
            Sleep(1);
            continue;
        }
        std::lock_guard<std::mutex> lock(g_lock);
        for (size_t i = 0; i < g_pending.size();) {
            auto it = g_sockets.find(g_pending[i].sock);
            bool ready = it == g_sockets.end() || FD_ISSET(it->second.s, &set);
            if (ready && TryComplete(g_pending[i]))
                g_pending.erase(g_pending.begin() + ptrdiff_t(i));
            else
                i++;
        }
    }
}

// an XNADDR for a system link at ip:port (network order)
void MakeXnAddr(uint8_t* x, const Endpoint& at) {
    std::memset(x, 0, kXnAddrSize);
    std::memcpy(x, &at.ip, 4);
    std::memcpy(x + kXnAddrPort, &at.port, 2);
    // Microsoft's OUI, then the address's last three bytes, the port's distance from the system link port mixed in
    const uint16_t d = uint16_t(ntohs(at.port) - kSystemLinkPort);
    const uint8_t mac[6] = {0x00, 0x50, 0xF2, uint8_t(at.ip >> 8), uint8_t((at.ip >> 16) ^ (d >> 8)),
                            uint8_t((at.ip >> 24) ^ d)};
    std::memcpy(x + kXnAddrEnet, mac, 6);
}
void OwnXnAddr(uint8_t* x) { MakeXnAddr(x, {g_local, htons(g_port)}); }

// the peer an XNADDR is, with g_peer_lock held: the one that brought it before, the one at its address, or for a
// LAN address from past the LAN the join address (on the client) or the peer past the LAN that hasn't brought one yet,
// the latest if no port matches (on the host). -1 when there is none to add.
int PeerFor(const uint8_t* x, const Endpoint& join) {
    for (size_t i = 0; i < g_peers.size(); i++)
        if (g_peers[i].has_xnaddr && std::memcmp(g_peers[i].xnaddr.data(), x, kXnAddrSize) == 0) return int(i);
    Endpoint at;
    std::memcpy(&at.ip, x, 4);
    std::memcpy(&at.port, x + kXnAddrPort, 2);
    if (!at.port) at.port = htons(kSystemLinkPort);  // not one of ours
    at.ip = Fold(at.ip);
    int i = FindPeer(at);
    if (i < 0 && Private(at.ip) && !g_instance) {
        if (join.ip && !Private(join.ip)) {
            i = AddPeer(join);
        } else {
            for (size_t j = 0; j < g_peers.size(); j++)
                if (!g_peers[j].has_xnaddr && !Private(g_peers[j].at.ip) && (i < 0 || g_peers[j].at.port == at.port))
                    i = int(j);
            if (i < 0 && g_last_remote >= 0 && !Private(g_peers[size_t(g_last_remote)].at.ip)) i = g_last_remote;
        }
    }
    return i >= 0 ? i : AddPeer(at);
}

void Random(uint32_t va, uint32_t n) {
    static std::mutex m;
    static std::mt19937 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lock(m);
    auto p = static_cast<uint8_t*>(HostPtr(va));
    for (uint32_t i = 0; i < n; i++) p[i] = uint8_t(rng());
}

}  // namespace

namespace xnet {

// for the menu, which holds UiLock and so must not start the network (Init takes UiLock): this PC's address on its
// LAN, and the port system link is on, 0 before the network starts
std::string LocalAddress() { return Ip(g_ready ? g_local : PrimaryAddress()); }
uint16_t Port() { return g_ready ? g_port : 0; }
int PortError() { return g_link_error; }  // why Port couldn't be bound (WSAEADDRINUSE: another program has it), 0 if it was

void CloseSockets() {
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& [id, socket] : g_sockets) ::closesocket(socket.s);
    g_sockets.clear();
    g_pending.clear();
}

}  // namespace xnet

HLE_STDCALL(XNetStartup, 1) {
    Init();
    return 0;
}
HLE_STDCALL(WSAStartup, 2) {
    Init();
    if (a[1]) {
        std::memset(HostPtr(a[1]), 0, 400);
        W32(a[1], 0x02020202);  // wVersion, wHighVersion
    }
    return 0;
}
HLE_STDCALL(XNetGetEthernetLinkStatus, 0) { return kLinkUp; }
HLE_STDCALL(XOnlineStartup, 1) { return 0; }
HLE_STDCALL(XOnlineCleanup, 0) { return 0; }
HLE_STDCALL(WSAGetLastError, 0) { return t_error; }

HLE_STDCALL(XNetRandom, 2) {
    Random(a[0], a[1]);
    return 0;
}
HLE_STDCALL(XNetCreateKey, 2) {
    Random(a[0], 8);
    Random(a[1], 16);
    kernel::G<uint8_t>(a[0]) &= 0x0F;
    return 0;
}
HLE_STDCALL(XNetRegisterKey, 2) {
    std::lock_guard<std::mutex> lock(g_lock);
    std::memcpy(g_kid.data(), HostPtr(a[0]), 8);
    return 0;
}
HLE_STDCALL(XNetUnregisterKey, 1) { return 0; }

HLE_STDCALL(XNetGetTitleXnAddr, 1) {
    Init();
    OwnXnAddr(static_cast<uint8_t*>(HostPtr(a[0])));
    return kXnAddrEthernet;
}
HLE_STDCALL(XNetXnAddrToInAddr, 3) {
    Init();
    const auto* x = static_cast<const uint8_t*>(HostPtr(a[0]));
    if (!R32(a[0])) return WSAEINVAL;
    uint8_t own[kXnAddrSize];
    OwnXnAddr(own);
    if (std::memcmp(x, own, kXnAddrSize) == 0) {
        W32(a[2], g_local);
        return 0;
    }
    Endpoint join = JoinAddress();
    std::lock_guard<std::mutex> lock(g_peer_lock);
    int i = PeerFor(x, join);
    if (i < 0) return WSAEINVAL;
    Peer& p = g_peers[size_t(i)];
    if (!p.has_xnaddr) {
        std::memcpy(p.xnaddr.data(), x, kXnAddrSize);
        p.has_xnaddr = true;
        LOGR("xnet: %s is the XNADDR %s:%u", Ip(Opaque(size_t(i))).c_str(), Ip(R32(a[0])).c_str(),
             ntohs(R16(a[0] + kXnAddrPort)));
    }
    W32(a[2], Opaque(size_t(i)));
    return 0;
}
HLE_STDCALL(XNetInAddrToXnAddr, 3) {
    Init();
    uint32_t ina = a[0];
    uint8_t x[kXnAddrSize];
    int i = PeerOf(ina);
    {
        std::lock_guard<std::mutex> lock(g_peer_lock);
        if (!ina || ina == g_local) OwnXnAddr(x);
        else if (i >= 0 && size_t(i) < g_peers.size() && g_peers[size_t(i)].has_xnaddr)
            std::memcpy(x, g_peers[size_t(i)].xnaddr.data(), kXnAddrSize);
        else if (i >= 0 && size_t(i) < g_peers.size()) MakeXnAddr(x, g_peers[size_t(i)].at);
        else MakeXnAddr(x, {ina, htons(kSystemLinkPort)});
    }
    if (a[1]) std::memcpy(HostPtr(a[1]), x, kXnAddrSize);
    std::lock_guard<std::mutex> lock(g_lock);
    if (a[2]) std::memcpy(HostPtr(a[2]), g_kid.data(), 8);
    return 0;
}

HLE_STDCALL(XNetDnsLookup, 3) {
    std::string name = kernel::ReadCString(a[0], 256);
    uint32_t dns = mem::AllocSystem(kXnDnsSize);
    std::memset(HostPtr(dns), 0, kXnDnsSize);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    uint32_t count = 0;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &res) == 0) {
        for (addrinfo* r = res; r && count < 8; r = r->ai_next)
            W32(dns + 8 + 4 * count++, reinterpret_cast<sockaddr_in*>(r->ai_addr)->sin_addr.s_addr);
        freeaddrinfo(res);
    }
    W32(dns, count ? 0 : WSAHOST_NOT_FOUND);
    W32(dns + 4, count);
    W32(a[2], dns);
    LOGI("xnet: dns %s -> %u addresses", name.c_str(), count);
    Signal(a[1]);
    return 0;
}
HLE_STDCALL(XNetDnsRelease, 1) {
    if (a[0]) mem::FreeSystem(a[0]);
    return 0;
}

static uint32_t AddSocket(SOCKET s, bool udp) {
    u_long on = 1;
    ::ioctlsocket(s, FIONBIO, &on);
    Socket k{s, udp};
    if (udp) {
        BOOL off = FALSE;
        DWORD ret = 0;
        WSAIoctl(s, kUdpConnReset, &off, sizeof(off), nullptr, 0, &ret, nullptr, nullptr);
        // its inbox: where LinkPump hands it what comes for its link port
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int len = sizeof(k.inbox);
        if (::bind(s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0 ||
            ::getsockname(s, reinterpret_cast<sockaddr*>(&k.inbox), &len) != 0) {
            int e = HostError();
            ::closesocket(s);
            return Fail(e);
        }
    }
    std::lock_guard<std::mutex> lock(g_lock);
    uint32_t id = g_next_socket;
    g_next_socket += 4;
    g_sockets[id] = k;
    return id;
}

namespace {

// with g_lock held: whether a UDP socket other than sock has link port vport
bool VportTaken(uint16_t vport, uint32_t sock) {
    for (auto& [id, k] : g_sockets)
        if (id != sock && k.udp && k.vport == vport) return true;
    return false;
}

uint16_t AutoBind(uint32_t sock) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_sockets.find(sock);
    if (it == g_sockets.end()) return 0;
    if (it->second.vport) return it->second.vport;
    uint16_t v;
    do {
        v = htons(g_next_vport);
        g_next_vport = g_next_vport == 0xFFFF ? kFirstEphemeral : uint16_t(g_next_vport + 1);
    } while (VportTaken(v, sock));
    it->second.vport = v;
    return v;
}

}  // namespace

HLE_STDCALL(socket, 3) {
    Init();
    uint32_t af = a[0], type = a[1];  // the protocol: UDP, or VDP, which is UDP between two copies of this
    if (af != AF_INET) return Fail(WSAEAFNOSUPPORT);
    bool udp = type == SOCK_DGRAM;
    if (!udp && type != SOCK_STREAM) return Fail(WSAESOCKTNOSUPPORT);
    SOCKET s = ::socket(AF_INET, int(type), udp ? IPPROTO_UDP : IPPROTO_TCP);
    if (s == INVALID_SOCKET) return Fail(HostError());
    return AddSocket(s, udp);
}
HLE_STDCALL(closesocket, 1) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_sockets.find(a[0]);
    if (it == g_sockets.end()) return Fail(WSAENOTSOCK);
    for (size_t i = 0; i < g_pending.size();) {
        if (g_pending[i].sock == a[0])
            g_pending.erase(g_pending.begin() + ptrdiff_t(i));
        else
            i++;
    }
    ::closesocket(it->second.s);
    g_sockets.erase(it);
    return 0;
}
HLE_STDCALL(shutdown, 2) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    return ::shutdown(k.s, int(a[1])) == 0 ? 0 : Fail(HostError());
}
HLE_STDCALL(ioctlsocket, 3) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_sockets.find(a[0]);
    if (it == g_sockets.end()) return Fail(WSAENOTSOCK);
    if (a[1] == uint32_t(FIONBIO)) {
        it->second.nonblocking = R32(a[2]) != 0;
        return 0;
    }
    u_long v = R32(a[2]);
    if (::ioctlsocket(it->second.s, long(a[1]), &v) != 0) return Fail(HostError());
    if (it->second.udp && a[1] == uint32_t(FIONREAD)) v = v >= kInboxHeader ? v - kInboxHeader : 0;
    W32(a[2], uint32_t(v));
    return 0;
}
HLE_STDCALL(setsockopt, 5) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    if (::setsockopt(k.s, int(a[1]), int(a[2]), static_cast<const char*>(HostPtr(a[3])), int(a[4])) == 0) return 0;
    LOG_FIRST(10, "xnet: setsockopt %X level %X option %X ignored (%d)", a[0], a[1], a[2], WSAGetLastError());
    return 0;
}
HLE_STDCALL(getsockopt, 5) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    int len = int(R32(a[4]));
    if (::getsockopt(k.s, int(a[1]), int(a[2]), static_cast<char*>(HostPtr(a[3])), &len) != 0) return Fail(WSAGetLastError());
    W32(a[4], uint32_t(len));
    return 0;
}
HLE_STDCALL(getsockname, 3) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    sockaddr_in me{};
    int len = sizeof(me);
    if (k.udp) {
        me.sin_family = AF_INET;
        me.sin_port = k.vport;
    } else if (::getsockname(k.s, reinterpret_cast<sockaddr*>(&me), &len) != 0) {
        return Fail(HostError());
    }
    std::memcpy(HostPtr(a[1]), &me, sizeof(me));
    W32(a[2], sizeof(me));
    return 0;
}
HLE_STDCALL(bind, 3) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    sockaddr_in at = GuestAddr(a[1]);
    if (k.udp) {
        if (!at.sin_port) {
            LOGR("xnet: bind %X to link port %u", a[0], ntohs(AutoBind(a[0])));
            return 0;
        }
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_sockets.find(a[0]);
        if (it == g_sockets.end()) return Fail(WSAENOTSOCK);
        if (VportTaken(at.sin_port, a[0])) {
            LOGR("xnet: bind %X to link port %u: in use", a[0], ntohs(at.sin_port));
            return Fail(WSAEADDRINUSE);
        }
        it->second.vport = at.sin_port;
        LOGR("xnet: bind %X to link port %u (on UDP %u)", a[0], ntohs(at.sin_port), g_port);
        return 0;
    }
    if (g_instance && at.sin_addr.s_addr == INADDR_ANY) at.sin_addr.s_addr = g_local;
    if (::bind(k.s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0) {
        int e = HostError();
        LOGR("xnet: bind %X (TCP) to %s failed (%d)", a[0], Addr(at).c_str(), e);
        return Fail(e);
    }
    LOGR("xnet: bind %X (TCP) to %s", a[0], Addr(at).c_str());
    return 0;
}
HLE_STDCALL(connect, 3) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    sockaddr_in to = GuestAddr(a[1]);
    if (k.udp) {  // where send goes, and nothing else: the link takes from anyone
        std::lock_guard<std::mutex> lock(g_lock);
        if (auto it = g_sockets.find(a[0]); it != g_sockets.end()) it->second.to = to;
        return 0;
    }
    to.sin_addr.s_addr = ToReal(to.sin_addr.s_addr).ip;
    if (::connect(k.s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) return 0;
    int e = HostError();
    if (e != WSAEWOULDBLOCK || k.nonblocking) return Fail(e);
    if (!Wait(k.s, true, 10000)) return Fail(WSAETIMEDOUT);
    int err = 0, len = sizeof(err);
    ::getsockopt(k.s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
    return err ? Fail(err) : 0;
}
HLE_STDCALL(listen, 2) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    return ::listen(k.s, int(a[1])) == 0 ? 0 : Fail(WSAGetLastError());
}
HLE_STDCALL(accept, 3) {
    for (;;) {
        Socket k = Find(a[0]);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        sockaddr_in from{};
        int len = sizeof(from);
        SOCKET s = ::accept(k.s, reinterpret_cast<sockaddr*>(&from), &len);
        if (s != INVALID_SOCKET) {
            if (a[1]) std::memcpy(HostPtr(a[1]), &from, sizeof(from));
            if (a[2]) W32(a[2], sizeof(from));
            LOGI("xnet: accept %X from %s", a[0], Addr(from).c_str());
            return AddSocket(s, false);
        }
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK || k.nonblocking) return Fail(e);
        Wait(k.s, false, 50);
    }
}
HLE_STDCALL(select, 5) {
    uint32_t guest[3] = {a[1], a[2], a[3]};
    fd_set host[3];
    std::vector<std::pair<uint32_t, SOCKET>> used[3];
    int total = 0;
    for (int i = 0; i < 3; i++) {
        FD_ZERO(&host[i]);
        if (!guest[i]) continue;
        uint32_t n = std::min(R32(guest[i]), kFdSetMax);
        for (uint32_t j = 0; j < n; j++) {
            uint32_t id = R32(guest[i] + 4 + 4 * j);
            Socket k = Find(id);
            if (k.s == INVALID_SOCKET) continue;
            FD_SET(k.s, &host[i]);
            used[i].push_back({id, k.s});
            total++;
        }
    }
    timeval tv{};
    timeval* ptv = nullptr;
    if (a[4]) {
        tv.tv_sec = long(R32(a[4]));
        tv.tv_usec = long(R32(a[4] + 4));
        ptv = &tv;
    }
    int r = 0;
    if (!total) {
        if (ptv) Sleep(DWORD(tv.tv_sec * 1000 + tv.tv_usec / 1000));
    } else {
        r = ::select(0, guest[0] ? &host[0] : nullptr, guest[1] ? &host[1] : nullptr, guest[2] ? &host[2] : nullptr, ptv);
        if (r < 0) return Fail(HostError());
    }
    for (int i = 0; i < 3; i++) {
        if (!guest[i]) continue;
        uint32_t k = 0;
        for (auto& [id, s] : used[i])
            if (FD_ISSET(s, &host[i])) W32(guest[i] + 4 + 4 * k++, id);
        W32(guest[i], k);
    }
    return uint32_t(r);
}

HLE_STDCALL(recvfrom, 6) {
    std::vector<Span> spans{{a[1], a[2]}};
    for (;;) {
        Socket k = Find(a[0]);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        int err = 0;
        int n;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            n = Recv(a[0], k, spans, a[4], a[5], err);
        }
        if (n >= 0) return uint32_t(n);
        if (err != WSAEWOULDBLOCK || k.nonblocking) return Fail(err);
        Wait(k.s, false, 50);
    }
}
// on a stream socket a reset connection is an error, not a stale port unreachable reply
HLE_STDCALL(recv, 4) {
    std::vector<Span> spans{{a[1], a[2]}};
    for (;;) {
        Socket k = Find(a[0]);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        if (k.udp) {
            int err = 0, n;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                n = Recv(a[0], k, spans, 0, 0, err);
            }
            if (n >= 0) return uint32_t(n);
            if (err != WSAEWOULDBLOCK || k.nonblocking) return Fail(err);
            Wait(k.s, false, 50);
            continue;
        }
        int n = ::recv(k.s, static_cast<char*>(HostPtr(a[1])), int(a[2]), int(a[3]));
        if (n >= 0) return uint32_t(n);
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK || k.nonblocking) return Fail(e);
        Wait(k.s, false, 50);
    }
}
HLE_STDCALL(send, 4) {
    for (;;) {
        Socket k = Find(a[0]);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        if (k.udp) {
            int err = 0;
            int n = Send(a[0], k, static_cast<const uint8_t*>(HostPtr(a[1])), int(a[2]), nullptr, err);
            return n < 0 ? Fail(err) : uint32_t(n);
        }
        int n = ::send(k.s, static_cast<const char*>(HostPtr(a[1])), int(a[2]), int(a[3]));
        if (n >= 0) return uint32_t(n);
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK || k.nonblocking) return Fail(e);
        Wait(k.s, true, 50);
    }
}
HLE_STDCALL(sendto, 6) {
    Socket k = Find(a[0]);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    sockaddr_in to{};
    if (a[4]) to = GuestAddr(a[4]);
    int err = 0;
    int n = Send(a[0], k, static_cast<const uint8_t*>(HostPtr(a[1])), int(a[2]), a[4] ? &to : nullptr, err);
    return n < 0 ? Fail(err) : uint32_t(n);
}

namespace {

uint32_t SendOverlapped(uint32_t sock, uint32_t bufs, uint32_t count, uint32_t sent, uint32_t to_va, uint32_t ovl) {
    Socket k = Find(sock);
    if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
    auto data = Gather(bufs, count);
    sockaddr_in to{};
    if (to_va) to = GuestAddr(to_va);
    int err = 0;
    int n = Send(sock, k, data.data(), int(data.size()), to_va ? &to : nullptr, err);
    if (n < 0) return Fail(err);
    if (sent) W32(sent, uint32_t(n));
    if (ovl) Complete(ovl, 0, uint32_t(n));
    return 0;
}

uint32_t RecvOverlapped(uint32_t sock, uint32_t bufs, uint32_t count, uint32_t received, uint32_t flags, uint32_t from,
                        uint32_t fromlen, uint32_t ovl) {
    PendingRecv p{sock, ovl, from, fromlen, Spans(bufs, count)};
    for (;;) {
        Socket k = Find(sock);
        if (k.s == INVALID_SOCKET) return Fail(WSAENOTSOCK);
        std::unique_lock<std::mutex> lock(g_lock);
        int err = 0;
        int n = Recv(sock, k, p.spans, from, fromlen, err);
        if (n >= 0) {
            if (received) W32(received, uint32_t(n));
            if (flags) W32(flags, 0);
            if (ovl) Complete(ovl, 0, uint32_t(n));
            return 0;
        }
        if (err != WSAEWOULDBLOCK) return Fail(err);
        if (ovl) {
            W32(ovl + kOvlStatus, kOvlPending);
            if (auto o = kernel::FromHandle(R32(ovl + kOvlEvent))) kernel::ResetEvent(*o);
            g_pending.push_back(std::move(p));
            return Fail(WSA_IO_PENDING);
        }
        if (k.nonblocking) return Fail(err);
        lock.unlock();
        Wait(k.s, false, 50);
    }
}

}  // namespace

HLE_STDCALL(WSARecvFrom, 9) { return RecvOverlapped(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); }
HLE_STDCALL(WSARecv, 7) { return RecvOverlapped(a[0], a[1], a[2], a[3], a[4], 0, 0, a[5]); }
HLE_STDCALL(WSASendTo, 9) { return SendOverlapped(a[0], a[1], a[2], a[3], a[5], a[7]); }
HLE_STDCALL(WSASend, 7) { return SendOverlapped(a[0], a[1], a[2], a[3], 0, a[5]); }

HLE_STDCALL(WSAGetOverlappedResult, 5) {
    uint32_t ovl = a[1];
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_lock);
            auto it = std::find_if(g_pending.begin(), g_pending.end(), [&](const PendingRecv& p) { return p.ovl == ovl; });
            if (it == g_pending.end() || TryComplete(*it)) {
                if (it != g_pending.end()) g_pending.erase(it);
                uint32_t status = R32(ovl + kOvlStatus);
                if (status == kOvlPending) {
                    t_error = WSA_IO_INCOMPLETE;
                    return 0;
                }
                if (a[2]) W32(a[2], R32(ovl + kOvlBytes));
                if (a[4]) W32(a[4], 0);
                if (status) {
                    t_error = status;
                    return 0;
                }
                return 1;
            }
        }
        if (!a[3]) {
            t_error = WSA_IO_INCOMPLETE;
            return 0;
        }
        Sleep(1);
    }
}
HLE_STDCALL(WSACancelOverlappedIO, 1) {
    std::lock_guard<std::mutex> lock(g_lock);
    for (size_t i = 0; i < g_pending.size();) {
        if (g_pending[i].sock == a[0]) {
            Complete(g_pending[i].ovl, WSA_OPERATION_ABORTED, 0);
            g_pending.erase(g_pending.begin() + ptrdiff_t(i));
        } else {
            i++;
        }
    }
    return 0;
}
