// System link: a malformed packet can no longer send the game's receive loop past its data or through a stray pointer.
//
// networklib's receive (0x419FA0) reads a datagram into a 0x500 byte buffer on its stack and unpacks the messages in
// it with the bit reader 0x41DFA0 (pack.cpp). The reader's state is at [esp+0x28]: +0 the bit in the byte, +4 the
// byte, +8 the buffer, +0xC its length, the bytes received. The reader checks none of it (no assert either), and the
// loop around it trusts the packet:
//   - the packet's length in bits ([esp+0x10], set at 0x41A519) is the bytes received, or, on a connection whose +0xC
//     is 0, a 16 bit length read from the packet (len * 8 + 16), never compared with what arrived: up to 64 KB of
//     the stack above the buffer read as messages;
//   - each message starts with its type in [0x84BE68] = 8 bits, and 0x416DD0 calls the factory at [0x84BE5C] + type
//     * 4 with the type checked against [0x84BE64] (0x4C, the table's size) only by an assert: a type of 0x4C or
//     more calls whatever pointer lies past the table, and a type with no factory leaves a null message that
//     0x41A58C reads through;
//   - the loop goes on while the bits left (0x41A6B8/0x41A6D2, unsigned) are 8 or more ([0x84BE8C] when set), so a
//     message read past the end leaves a negative count that keeps it going through whatever follows.
// The guards here: the length is held to the bytes received (0x41A519), the loop stops when fewer bits than a type's
// are left, counted signed (0x41A540), a type with no factory ends the packet (0x41A55A), and a message whose
// reading ran past the end is deleted, not delivered, and ends the packet (0x41A68F). The loop's exit 0x41A6F8 then
// runs as after the last message. A packet the game itself sent never meets any of them. XBE_NET_TRACE=1 logs each.
#include <cstdint>
#include <cstdlib>

#include "core/log.h"
#include "core/diag.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {

constexpr uint32_t kTypeCount = 0x84BE64;  // the factory table's entries
constexpr uint32_t kTypeBits = 0x84BE68;   // a message type's bits
constexpr uint32_t kFactories = 0x84BE5C;  // -> the factory table

// the receive's frame (0x419FA0, esp as in its loop)
constexpr uint32_t kTotalBits = 0x10;   // the packet's length in bits
constexpr uint32_t kBit = 0x28;         // the reader: the bit in the byte
constexpr uint32_t kByte = 0x2C;        //             the byte
constexpr uint32_t kReceived = 0x34;    //             the bytes received

bool Trace() {
    static const bool on = diag::Env("XBE_NET_TRACE") != nullptr;
    return on;
}

// the bits read so far, past the packet's length when positive
int64_t Over(const X86Context& ctx) {
    const int64_t at = int64_t(kernel::R32(ctx.esp + kByte)) * 8 + kernel::R32(ctx.esp + kBit);
    return at - int64_t(kernel::R32(ctx.esp + kTotalBits));
}

}  // namespace

// [[hook]] at 0x41A519 in the receive (0x419FA0), eax the packet's length in bits: no more than arrived
void ConkerNetLength(X86Context& ctx, uint8_t* base) {
    (void)base;
    const uint32_t received = kernel::R32(ctx.esp + kReceived) * 8;
    if (ctx.eax <= received) return;
    if (Trace()) LOGR("net guard: a packet claims %u bits, %u arrived", ctx.eax, received);
    ctx.eax = received;
}

// [[hook]] at 0x41A540, the message loop's top: true (to its exit) when a message's type no longer fits
bool ConkerNetLoop(X86Context& ctx, uint8_t* base) {
    (void)base;
    const int64_t left = -Over(ctx);
    if (left >= int64_t(kernel::R32(kTypeBits))) return false;
    if (left < 0 && Trace()) LOGR("net guard: a packet read %lld bits past its end", (long long)-left);
    return true;
}

// [[hook]] at 0x41A55A, eax the type just read: true (to the loop's exit) for a type with no factory
bool ConkerNetType(X86Context& ctx, uint8_t* base) {
    (void)base;
    const uint32_t type = ctx.eax, table = kernel::R32(kFactories);
    if (table && type < kernel::R32(kTypeCount) && kernel::R32(table + type * 4)) return false;
    if (Trace()) LOGR("net guard: a packet's message type %u has no factory", type);
    return true;
}

// [[hook]] at 0x41A68F, the message (edi) read: true (to its delete and the loop's exit) when it ran past the end
bool ConkerNetRead(X86Context& ctx, uint8_t* base) {
    (void)base;
    const int64_t over = Over(ctx);
    if (over <= 0) return false;
    if (Trace()) LOGR("net guard: a message read %lld bits past its packet, dropped", (long long)over);
    return true;
}
