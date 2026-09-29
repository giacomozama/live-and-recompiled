#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "app/app.h"
#include "kernel/crypto.h"
#include "kernel/exports.h"
#include "xbe/xbe.h"

namespace kernel {

extern const xbe::Image* g_image;

namespace {

KERNEL_DATA(156, KeTickCount, 4);
KERNEL_DATA(164, LaunchDataPage, 4);
KERNEL_DATA(322, XboxHardwareInfo, 8);
KERNEL_DATA(324, XboxKrnlVersion, 8);
KERNEL_DATA(326, XeImageFileName, 8 + 64);
KERNEL_DATA(41, HalDiskModelNumber, 8 + 48);
KERNEL_DATA(42, HalDiskSerialNumber, 8 + 32);
KERNEL_DATA(40, HalDiskCachePartitionCount, 4);
KERNEL_DATA(356, HalBootSMCVideoMode, 4);
KERNEL_DATA(323, XboxHDKey, 16);
KERNEL_DATA(325, XboxSignatureKey, 16);
KERNEL_DATA(353, XboxLANKey, 16);
KERNEL_DATA(354, XboxAlternateSignatureKeys, 16 * 16);
KERNEL_DATA(355, XePublicKeyData, 284);
KERNEL_DATA(16, ExEventObjectType, 0x20);
KERNEL_DATA(71, IoFileObjectType, 0x20);
KERNEL_DATA(259, PsThreadObjectType, 0x20);
KERNEL_DATA(357, IdexChannelObject, 0x40);

void PutAnsi(uint32_t str, const char* text) {
    uint32_t buf = str + 8;
    size_t n = std::strlen(text);
    std::memcpy(HostPtr(buf), text, n + 1);
    W16(str, uint16_t(n));
    W16(str + 2, uint16_t(n + 1));
    W32(str + 4, buf);
}

uint8_t& Irql() { return G<uint8_t>(Current()->kpcr + 0x24); }

int64_t TimeoutArg(uint32_t ptr) { return ptr ? int64_t(R64(ptr)) : INT64_MAX; }

}  // namespace

uint32_t g_tick_count_va = 0;
uint32_t g_launch_data_page_va = 0;

void InitKernelData() {
    g_tick_count_va = DataAddress(156);
    g_launch_data_page_va = DataAddress(164);
    W32(DataAddress(164), 0);
    uint32_t hw = DataAddress(322);
    W32(hw, 0x00000020);
    G<uint8_t>(hw + 4) = 0xD3;  // GPU revision
    G<uint8_t>(hw + 5) = 0xB1;  // MCP revision
    uint32_t ver = DataAddress(324);
    W16(ver, 1);
    W16(ver + 2, 0);
    W16(ver + 4, 5838);
    W16(ver + 6, 1);
    PutAnsi(DataAddress(326), "\\Device\\CdRom0\\default.xbe");
    PutAnsi(DataAddress(41), "RECOMP HARD DISK");
    PutAnsi(DataAddress(42), "0000000000000000");
    W32(DataAddress(40), 3);
    W32(DataAddress(356), 0);
    // IDE_CHANNEL_OBJECT DeviceQueue.DeviceListHead, XAPI walks it to cancel file I/O
    uint32_t queue = DataAddress(357) + 0x28;
    W32(queue, queue);
    W32(queue + 4, queue);
    // the start of the clock
    std::thread([] {
        auto start = std::chrono::steady_clock::now();
        for (;;) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            W32(g_tick_count_va, uint32_t(ms));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
}

// ---------------------------------------------------------------- critical sections

KERNEL_EXPORT(291, RtlInitializeCriticalSection, 1) {
    uint32_t cs = a[0];
    G<uint8_t>(cs) = 1;  // SynchronizationEvent
    G<uint8_t>(cs + 1) = 0;
    G<uint8_t>(cs + 2) = 4;
    G<uint8_t>(cs + 3) = 0;
    W32(cs + 4, 0);
    W32(cs + 8, cs + 8);
    W32(cs + 12, cs + 8);
    W32(cs + 0x10, 0xFFFFFFFF);  // LockCount
    W32(cs + 0x14, 0);           // RecursionCount
    W32(cs + 0x18, 0);           // OwningThread
    return 0;
}

KERNEL_EXPORT(277, RtlEnterCriticalSection, 1) {
    uint32_t cs = a[0];
    uint32_t me = Current()->ethread;
    auto* lock_count = reinterpret_cast<int32_t*>(HostPtr(cs + 0x10));
    if (__atomic_add_fetch(lock_count, 1, __ATOMIC_SEQ_CST) == 0) {
        W32(cs + 0x18, me);
        W32(cs + 0x14, 1);
        return 0;
    }
    if (R32(cs + 0x18) == me) {
        W32(cs + 0x14, R32(cs + 0x14) + 1);
        return 0;
    }
    ObjectRef ev = FromPointer(cs);
    Wait({ev}, false, INT64_MAX, false);
    W32(cs + 0x18, me);
    W32(cs + 0x14, 1);
    return 0;
}

KERNEL_EXPORT(294, RtlLeaveCriticalSection, 1) {
    uint32_t cs = a[0];
    auto* lock_count = reinterpret_cast<int32_t*>(HostPtr(cs + 0x10));
    uint32_t rec = R32(cs + 0x14) - 1;
    W32(cs + 0x14, rec);
    if (rec) {
        __atomic_sub_fetch(lock_count, 1, __ATOMIC_SEQ_CST);
        return 0;
    }
    W32(cs + 0x18, 0);
    if (__atomic_sub_fetch(lock_count, 1, __ATOMIC_SEQ_CST) >= 0) SetEvent(*FromPointer(cs));
    return 0;
}

// ---------------------------------------------------------------- irql, time

KERNEL_FASTCALL(160, KfRaiseIrql) {
    uint8_t old = Irql();
    Irql() = uint8_t(ctx.ecx);
    return old;
}

KERNEL_FASTCALL(161, KfLowerIrql) {
    Irql() = uint8_t(ctx.ecx);
    return 0;
}

KERNEL_EXPORT(129, KeRaiseIrqlToDpcLevel, 0) {
    uint8_t old = Irql();
    Irql() = 2;
    return old;
}

KERNEL_EXPORT(128, KeQuerySystemTime, 1) {
    W64(a[0], uint64_t(Now100ns()));
    return 0;
}

KERNEL_EXPORT(228, NtSetSystemTime, 2) {
    if (a[1]) W64(a[1], uint64_t(Now100ns()));
    return kSuccess;
}

// the console's ACPI timer
constexpr uint64_t kPerfFrequency = 3375000;

KERNEL_EXPORT(126, KeQueryPerformanceCounter, 0) {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    uint64_t ns = uint64_t(duration_cast<nanoseconds>(steady_clock::now() - start).count());
    uint64_t v = ns / 1000000000ull * kPerfFrequency + ns % 1000000000ull * kPerfFrequency / 1000000000ull;
    ctx.edx = uint32_t(v >> 32);
    return uint32_t(v);
}

KERNEL_EXPORT(127, KeQueryPerformanceFrequency, 0) {
    ctx.edx = 0;
    return uint32_t(kPerfFrequency);
}

KERNEL_EXPORT(151, KeStallExecutionProcessor, 1) {
    auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(a[0]);
    while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
    return 0;
}

KERNEL_EXPORT(99, KeDelayExecutionThread, 3) {
    int64_t interval = int64_t(R64(a[2]));
    bool alertable = a[1] & 0xFF;
    Thread* t = Current();
    if (alertable && t && RunApcs(*t)) return kUserApc;
    int64_t deadline = ToDeadline(interval);
    auto now = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    if (interval == 0) {
        std::this_thread::yield();
        return kSuccess;
    }
    while (now() < deadline) {
        int64_t left = deadline - now();
        std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(left, 2000000)));
        if (alertable && t && RunApcs(*t)) return kUserApc;
    }
    return kSuccess;
}

static int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

KERNEL_EXPORT(304, RtlTimeFieldsToTime, 2) {
    uint32_t tf = a[0];
    int64_t days = DaysFromCivil(int16_t(R16(tf)), R16(tf + 2), R16(tf + 4));
    int64_t secs = days * 86400 + R16(tf + 6) * 3600 + R16(tf + 8) * 60 + R16(tf + 10);
    int64_t t = (secs + 11644473600ll) * 10000000ll + int64_t(R16(tf + 12)) * 10000;
    W64(a[1], uint64_t(t));
    return 1;
}

KERNEL_EXPORT(305, RtlTimeToTimeFields, 2) {
    int64_t t = int64_t(R64(a[0]));
    int64_t ms = (t / 10000) % 1000;
    int64_t secs = t / 10000000 - 11644473600ll;
    int64_t days = secs >= 0 ? secs / 86400 : (secs - 86399) / 86400;
    int64_t rem = secs - days * 86400;
    // civil from days
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = int64_t(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    uint32_t tf = a[1];
    W16(tf, uint16_t(y));
    W16(tf + 2, uint16_t(m));
    W16(tf + 4, uint16_t(d));
    W16(tf + 6, uint16_t(rem / 3600));
    W16(tf + 8, uint16_t(rem / 60 % 60));
    W16(tf + 10, uint16_t(rem % 60));
    W16(tf + 12, uint16_t(ms));
    W16(tf + 14, uint16_t((days + 4) % 7 < 0 ? (days + 4) % 7 + 7 : (days + 4) % 7));
    return 0;
}

// ---------------------------------------------------------------- memory

KERNEL_EXPORT(184, NtAllocateVirtualMemory, 5) {
    uint32_t base = R32(a[0]), size = R32(a[2]);
    uint32_t st = mem::AllocateVirtual(base, size, a[3], a[4]);
    if (st == 0) {
        W32(a[0], base);
        W32(a[2], size);
    } else {
        LOGW("NtAllocateVirtualMemory %08X+%X type %X failed %08X", R32(a[0]), R32(a[2]), a[3], st);
    }
    return st;
}

KERNEL_EXPORT(199, NtFreeVirtualMemory, 3) {
    uint32_t base = R32(a[0]), size = R32(a[1]);
    uint32_t st = mem::FreeVirtual(base, size, a[2]);
    if (st == 0) {
        W32(a[0], base);
        W32(a[1], size);
    }
    return st;
}

KERNEL_EXPORT(217, NtQueryVirtualMemory, 2) {
    mem::RegionInfo ri;
    if (!mem::QueryVirtual(a[0], ri)) return kInvalidParameter;
    uint32_t o = a[1];
    W32(o, ri.base);
    W32(o + 4, ri.alloc_base);
    W32(o + 8, ri.alloc_protect);
    W32(o + 12, ri.size);
    W32(o + 16, ri.state);
    W32(o + 20, ri.protect);
    W32(o + 24, ri.type);
    return kSuccess;
}

KERNEL_EXPORT(165, MmAllocateContiguousMemory, 1) { return mem::AllocContiguous(a[0], 0, 0xFFFFFFFF, 0); }
KERNEL_EXPORT(166, MmAllocateContiguousMemoryEx, 5) { return mem::AllocContiguous(a[0], a[1], a[2], a[3]); }
KERNEL_EXPORT(171, MmFreeContiguousMemory, 1) {
    mem::FreeContiguous(a[0]);
    return 0;
}
KERNEL_EXPORT(173, MmGetPhysicalAddress, 1) { return mem::PhysicalFromVirtual(a[0]); }
KERNEL_EXPORT(178, MmPersistContiguousMemory, 3) { return 0; }
KERNEL_EXPORT(175, MmLockUnlockBufferPages, 3) { return 0; }
KERNEL_EXPORT(176, MmLockUnlockPhysicalPage, 2) { return 0; }
KERNEL_EXPORT(180, MmQueryAllocationSize, 1) { return mem::AllocationSize(a[0]); }
KERNEL_EXPORT(182, MmSetAddressProtect, 3) { return 0; }
KERNEL_EXPORT(179, MmQueryAddressProtect, 1) { return 4; }
KERNEL_EXPORT(167, MmAllocateSystemMemory, 2) { return mem::AllocSystem(a[0], 0x1000); }
KERNEL_EXPORT(172, MmFreeSystemMemory, 2) {
    mem::FreeSystem(a[0]);
    return a[1];
}
KERNEL_EXPORT(168, MmClaimGpuInstanceMemory, 2) {
    if (a[1]) W32(a[1], 0);
    return mem::AllocContiguous(a[0], 0, 0xFFFFFFFF, 0x1000) + a[0];
}
KERNEL_EXPORT(169, MmCreateKernelStack, 2) { return mem::AllocStack(a[0]); }
KERNEL_EXPORT(170, MmDeleteKernelStack, 2) {
    mem::FreeStack(a[0]);
    return 0;
}
KERNEL_EXPORT(15, ExAllocatePoolWithTag, 2) { return mem::AllocSystem(a[0]); }
KERNEL_EXPORT(17, ExFreePool, 1) {
    mem::FreeSystem(a[0]);
    return 0;
}
KERNEL_EXPORT(23, ExQueryPoolBlockSize, 1) { return mem::AllocationSize(a[0]); }

KERNEL_EXPORT(181, MmQueryStatistics, 1) {
    uint32_t o = a[0];
    if (R32(o) != 36) return kInvalidParameter;
    mem::Stats s = mem::Query();
    W32(o + 4, s.total_pages);
    W32(o + 8, s.available_pages);
    W32(o + 12, s.virtual_committed * mem::kPage);
    W32(o + 16, s.virtual_committed * mem::kPage);
    W32(o + 20, 0);
    W32(o + 24, s.system_used);
    W32(o + 28, 0);
    W32(o + 32, 0);
    return kSuccess;
}

KERNEL_EXPORT(327, XeLoadSection, 1) {
    uint32_t sec = a[0];
    uint32_t refs = R32(sec + 0x18);
    if (refs == 0) {
        uint32_t va = R32(sec + 4), vsize = R32(sec + 8), raw = R32(sec + 12), rsize = R32(sec + 16);
        mem::Commit(va, vsize);
        std::memset(HostPtr(va), 0, vsize);
        if (g_image && raw + rsize <= g_image->file.size())
            std::memcpy(HostPtr(va), g_image->file.data() + raw, std::min(rsize, vsize));
        LOGI("XeLoadSection %s at %08X", ReadCString(R32(sec + 0x14)).c_str(), va);
    }
    W32(sec + 0x18, refs + 1);
    return kSuccess;
}

KERNEL_EXPORT(328, XeUnloadSection, 1) {
    uint32_t sec = a[0];
    uint32_t refs = R32(sec + 0x18);
    if (!refs) return kInvalidParameter;
    W32(sec + 0x18, refs - 1);
    return kSuccess;
}

// ---------------------------------------------------------------- objects and waits

KERNEL_EXPORT(187, NtClose, 1) { return CloseHandle(a[0]) ? kSuccess : kInvalidHandle; }

KERNEL_EXPORT(197, NtDuplicateObject, 3) {
    uint32_t h = DuplicateHandle(a[0]);
    if (!h) return kInvalidHandle;
    W32(a[1], h);
    if (a[2] & 1) CloseHandle(a[0]);
    return kSuccess;
}

static ObjectRef Named(uint32_t attrs, std::string& name) {
    name = attrs ? ReadAnsiString(R32(attrs + 4)) : std::string();
    return name.empty() ? nullptr : FindNamed(name);
}

KERNEL_EXPORT(189, NtCreateEvent, 4) {
    std::string name;
    if (ObjectRef existing = Named(a[1], name)) {
        W32(a[0], NewHandle(existing));
        return kObjectNameExists;
    }
    auto o = std::make_shared<Object>(ObjType::Event);
    o->notification = a[2] == 0;
    o->signal = (a[3] & 0xFF) ? 1 : 0;
    uint32_t body = mem::AllocSystem(16);
    G<uint8_t>(body) = uint8_t(a[2]);
    G<uint8_t>(body + 2) = 4;
    W32(body + 4, uint32_t(o->signal));
    Bind(o, body);
    if (!name.empty()) AddNamed(name, o);
    W32(a[0], NewHandle(o));
    return kSuccess;
}

KERNEL_EXPORT(225, NtSetEvent, 2) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    int32_t prev = SetEvent(*o);
    if (a[1]) W32(a[1], uint32_t(prev));
    return kSuccess;
}

KERNEL_EXPORT(186, NtClearEvent, 1) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    ResetEvent(*o);
    return kSuccess;
}

KERNEL_EXPORT(192, NtCreateMutant, 3) {
    std::string name;
    if (ObjectRef existing = Named(a[1], name)) {
        W32(a[0], NewHandle(existing));
        return kObjectNameExists;
    }
    auto o = std::make_shared<Object>(ObjType::Mutant);
    uint32_t body = mem::AllocSystem(32);
    G<uint8_t>(body) = 2;
    G<uint8_t>(body + 2) = 8;
    if (a[2] & 0xFF) {
        o->owner = Current()->ethread;
        o->recursion = 1;
        o->signal = 0;
    } else {
        o->signal = 1;
    }
    W32(body + 4, uint32_t(o->signal));
    Bind(o, body);
    if (!name.empty()) AddNamed(name, o);
    W32(a[0], NewHandle(o));
    return kSuccess;
}

KERNEL_EXPORT(221, NtReleaseMutant, 2) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    uint32_t st;
    int32_t prev = ReleaseMutant(*o, st);
    if (a[1]) W32(a[1], uint32_t(prev));
    return st;
}

KERNEL_EXPORT(233, NtWaitForSingleObject, 3) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    return Wait({o}, false, TimeoutArg(a[2]), a[1] & 0xFF);
}

KERNEL_EXPORT(234, NtWaitForSingleObjectEx, 4) {
    ObjectRef o = FromHandle(a[0]);
    if (!o) return kInvalidHandle;
    return Wait({o}, false, TimeoutArg(a[3]), a[2] & 0xFF);
}

KERNEL_EXPORT(235, NtWaitForMultipleObjectsEx, 6) {
    std::vector<ObjectRef> objs;
    for (uint32_t i = 0; i < a[0]; i++) {
        ObjectRef o = FromHandle(R32(a[1] + i * 4));
        if (!o) return kInvalidHandle;
        objs.push_back(o);
    }
    return Wait(objs, a[2] == 0, TimeoutArg(a[5]), a[4] & 0xFF);
}

KERNEL_EXPORT(159, KeWaitForSingleObject, 5) {
    ObjectRef o = FromPointer(a[0]);
    return Wait({o}, false, TimeoutArg(a[4]), a[3] & 0xFF);
}

KERNEL_EXPORT(158, KeWaitForMultipleObjects, 8) {
    std::vector<ObjectRef> objs;
    for (uint32_t i = 0; i < a[0]; i++) objs.push_back(FromPointer(R32(a[1] + i * 4)));
    return Wait(objs, a[2] == 0, TimeoutArg(a[6]), a[5] & 0xFF);
}

KERNEL_EXPORT(145, KeSetEvent, 3) { return uint32_t(SetEvent(*FromPointer(a[0]))); }

KERNEL_EXPORT(246, ObReferenceObjectByHandle, 3) {
    uint32_t h = a[0];
    ObjectRef o = h == 0xFFFFFFFE ? CurrentRef() : FromHandle(h);
    if (!o) return kInvalidHandle;
    if (!o->guest) {
        uint32_t body = mem::AllocSystem(32);
        Bind(o, body);
    }
    W32(a[2], o->guest);
    return kSuccess;
}

KERNEL_EXPORT(247, ObReferenceObjectByName, 5) {
    std::string name = ReadAnsiString(a[0]);
    LOG_FIRST(8, "ObReferenceObjectByName %s", name.c_str());
    return kObjectNameNotFound;
}

KERNEL_FASTCALL(250, ObfDereferenceObject) { return 0; }

// ---------------------------------------------------------------- timers, DPCs, interrupts

KERNEL_EXPORT(113, KeInitializeTimerEx, 2) {
    uint32_t t = a[0];
    std::memset(HostPtr(t), 0, 0x28);
    G<uint8_t>(t) = uint8_t(8 + a[1]);
    G<uint8_t>(t + 2) = 10;
    W32(t + 8, t + 8);
    W32(t + 12, t + 8);
    return 0;
}

KERNEL_EXPORT(150, KeSetTimerEx, 5) {
    ObjectRef t = FromPointer(a[0]);
    bool was = CancelTimer(*t);
    int64_t due = int64_t(uint64_t(a[1]) | uint64_t(a[2]) << 32);
    W32(a[0] + 0x20, a[4]);
    W32(a[0] + 0x24, a[3]);
    SetTimer(t, due, int32_t(a[3]), a[4]);
    return was;
}

KERNEL_EXPORT(149, KeSetTimer, 4) {
    ObjectRef t = FromPointer(a[0]);
    bool was = CancelTimer(*t);
    int64_t due = int64_t(uint64_t(a[1]) | uint64_t(a[2]) << 32);
    SetTimer(t, due, 0, a[3]);
    return was;
}

KERNEL_EXPORT(97, KeCancelTimer, 1) { return CancelTimer(*FromPointer(a[0])); }

KERNEL_EXPORT(107, KeInitializeDpc, 3) {
    uint32_t d = a[0];
    std::memset(HostPtr(d), 0, 0x1C);
    W16(d, 19);
    W32(d + 0x0C, a[1]);
    W32(d + 0x10, a[2]);
    return 0;
}

KERNEL_EXPORT(119, KeInsertQueueDpc, 3) {
    uint32_t d = a[0];
    if (G<uint8_t>(d + 2)) return 0;
    G<uint8_t>(d + 2) = 1;
    QueueDpc(d, a[1], a[2]);
    return 1;
}

KERNEL_EXPORT(137, KeRemoveQueueDpc, 1) {
    uint8_t was = G<uint8_t>(a[0] + 2);
    G<uint8_t>(a[0] + 2) = 0;
    return was;
}

KERNEL_EXPORT(109, KeInitializeInterrupt, 7) { return 0; }
KERNEL_EXPORT(98, KeConnectInterrupt, 1) { return 1; }
KERNEL_EXPORT(100, KeDisconnectInterrupt, 1) { return 1; }
KERNEL_EXPORT(44, HalGetInterruptVector, 2) {
    if (a[1]) G<uint8_t>(a[1]) = uint8_t(a[0]);
    return a[0] + 0x30;
}
KERNEL_EXPORT(153, KeSynchronizeExecution, 3) { return CallGuest(ctx, a[1], {a[2]}) & 0xFF; }

// ---------------------------------------------------------------- threads

KERNEL_EXPORT(255, PsCreateSystemThreadEx, 10) {
    uint32_t handle_out = a[0], stack = a[2], tls = a[3], id_out = a[4], start = a[5], context = a[6];
    bool suspended = a[7] & 0xFF;
    uint32_t system = a[9];
    ThreadRef t = CreateThread(stack ? stack : 0x10000, tls, system, start, context, suspended);
    if (handle_out) W32(handle_out, NewHandle(t));
    if (id_out) W32(id_out, t->id);
    StartThread(t);
    return kSuccess;
}

KERNEL_EXPORT(258, PsTerminateSystemThread, 1) { ExitThread(a[0]); }

KERNEL_EXPORT(224, NtResumeThread, 2) {
    ObjectRef o = FromHandle(a[0]);
    if (!o || o->type != ObjType::Thread) return kInvalidHandle;
    auto& t = static_cast<Thread&>(*o);
    if (a[1]) W32(a[1], uint32_t(t.suspend.load()));
    Resume(t);
    return kSuccess;
}

KERNEL_EXPORT(143, KeSetBasePriorityThread, 2) {
    ObjectRef o = FromPointer(a[0]);
    if (!o || o->type != ObjType::Thread) return 0;
    auto& t = static_cast<Thread&>(*o);
    int old = t.priority;
    t.priority = int32_t(a[1]);
    return uint32_t(old);
}

KERNEL_EXPORT(139, KeRestoreFloatingPointState, 1) { return kSuccess; }
KERNEL_EXPORT(142, KeSaveFloatingPointState, 1) { return kSuccess; }

// ---------------------------------------------------------------- strings

KERNEL_EXPORT(289, RtlInitAnsiString, 2) {
    uint32_t d = a[0], s = a[1];
    size_t n = s ? std::strlen(reinterpret_cast<const char*>(HostPtr(s))) : 0;
    W16(d, uint16_t(n));
    W16(d + 2, uint16_t(s ? n + 1 : 0));
    W32(d + 4, s);
    return 0;
}

KERNEL_EXPORT(290, RtlInitUnicodeString, 2) {
    uint32_t d = a[0], s = a[1];
    size_t n = 0;
    if (s)
        while (R16(s + uint32_t(n) * 2)) n++;
    W16(d, uint16_t(n * 2));
    W16(d + 2, uint16_t(s ? n * 2 + 2 : 0));
    W32(d + 4, s);
    return 0;
}

KERNEL_EXPORT(260, RtlAnsiStringToUnicodeString, 3) {
    uint32_t d = a[0], s = a[1];
    uint16_t n = R16(s);
    uint32_t src = R32(s + 4);
    if (a[2] & 0xFF) {
        uint32_t buf = mem::AllocSystem(n * 2u + 2);
        W32(d + 4, buf);
        W16(d + 2, uint16_t(n * 2 + 2));
    } else if (R16(d + 2) < n * 2) {
        return kBufferTooSmall;
    }
    uint32_t buf = R32(d + 4);
    for (uint16_t i = 0; i < n; i++) W16(buf + i * 2u, G<uint8_t>(src + i));
    if (R16(d + 2) >= n * 2 + 2) W16(buf + n * 2u, 0);
    W16(d, uint16_t(n * 2));
    return kSuccess;
}

KERNEL_EXPORT(308, RtlUnicodeStringToAnsiString, 3) {
    uint32_t d = a[0], s = a[1];
    uint16_t n = R16(s) / 2;
    uint32_t src = R32(s + 4);
    if (a[2] & 0xFF) {
        uint32_t buf = mem::AllocSystem(n + 1u);
        W32(d + 4, buf);
        W16(d + 2, uint16_t(n + 1));
    } else if (R16(d + 2) < n) {
        return kBufferTooSmall;
    }
    uint32_t buf = R32(d + 4);
    for (uint16_t i = 0; i < n; i++) {
        uint16_t c = R16(src + i * 2u);
        G<uint8_t>(buf + i) = c < 0x100 ? uint8_t(c) : '?';
    }
    if (R16(d + 2) > n) G<uint8_t>(buf + n) = 0;
    W16(d, n);
    return kSuccess;
}

KERNEL_EXPORT(286, RtlFreeAnsiString, 1) {
    uint32_t buf = R32(a[0] + 4);
    if (buf) mem::FreeSystem(buf);
    W32(a[0] + 4, 0);
    W16(a[0], 0);
    W16(a[0] + 2, 0);
    return 0;
}

static int CompareStrings(uint32_t s1, uint32_t s2, bool ci) {
    std::string a1 = ReadAnsiString(s1), a2 = ReadAnsiString(s2);
    size_t n = std::min(a1.size(), a2.size());
    for (size_t i = 0; i < n; i++) {
        int c1 = uint8_t(a1[i]), c2 = uint8_t(a2[i]);
        if (ci) {
            c1 = std::toupper(c1);
            c2 = std::toupper(c2);
        }
        if (c1 != c2) return c1 - c2;
    }
    return int(a1.size()) - int(a2.size());
}

KERNEL_EXPORT(270, RtlCompareString, 3) { return uint32_t(CompareStrings(a[0], a[1], a[2] & 0xFF)); }
KERNEL_EXPORT(279, RtlEqualString, 3) { return CompareStrings(a[0], a[1], a[2] & 0xFF) == 0; }
KERNEL_EXPORT(316, RtlUpperChar, 1) { return uint32_t(std::toupper(int(a[0] & 0xFF))) & 0xFF; }
KERNEL_EXPORT(313, RtlUpcaseUnicodeChar, 1) {
    uint16_t c = uint16_t(a[0]);
    return c < 0x80 ? uint32_t(std::toupper(c)) : c;
}

KERNEL_EXPORT(269, RtlCompareMemoryUlong, 3) {
    uint32_t src = a[0], len = a[1] & ~3u, pat = a[2], n = 0;
    while (n < len && R32(src + n) == pat) n += 4;
    return n;
}

KERNEL_EXPORT(301, RtlNtStatusToDosError, 1) {
    switch (a[0]) {
        case kSuccess: return 0;
        case kPending: return 997;
        case kTimeout: return 1460;
        case kNoSuchFile:
        case kObjectNameNotFound: return 2;
        case kObjectPathNotFound: return 3;
        case kAccessDenied:
        case kCannotDelete:
        case kFileIsADirectory: return 5;
        case kInvalidHandle: return 6;
        case kNoMemory: return 8;
        case kNoMoreFiles: return 18;
        case kEndOfFile: return 38;
        case kNotSupported: return 50;
        case kInvalidParameter: return 87;
        case kNotImplemented: return 120;
        case kBufferTooSmall: return 122;
        case kObjectNameInvalid: return 123;
        case kObjectNameCollision: return 183;
        case kNotADirectory: return 267;
        case 0xC000007F: return 112;  // disk full
        case kDeviceNotConnected: return 1167;
        default: return (a[0] & 0xC0000000) ? 317 : 0;
    }
}

// ---------------------------------------------------------------- exceptions, debug, shutdown

// Exceptions dispatch like NT on x86: each handler of the fs:[0] chain is called (record, frame, context,
// dispatcher context) until one returns ExceptionContinueExecution. A C++ catch never returns: the CRT jumps to the
// continuation, which unwinds the host stack (X86Resume).
namespace {
constexpr uint32_t kContextBytes = 0x400, kUnwinding = 2, kExitUnwind = 4, kContinueExecution = 0;

uint32_t CallHandler(X86Context& ctx, uint32_t rec, uint32_t frame, uint32_t scratch) {
    return CallGuest(ctx, R32(frame + 4), {rec, frame, scratch, scratch + kContextBytes - 8});
}
}  // namespace

KERNEL_EXPORT(302, RtlRaiseException, 1) {
    uint32_t rec = a[0];
    std::string text;
    // a C++ throw carrying a wide string: the text
    if (R32(rec) == 0xE06D7363 && R32(rec + 16) >= 2 && mem::IsCommitted(R32(rec + 24))) {
        for (uint32_t p = R32(R32(rec + 24)), n = 0; mem::IsCommitted(p) && n < 256; n++, p += 2) {
            uint16_t c = R16(p);
            if (!c) break;
            text += c < 0x80 ? char(c) : '?';
        }
        LOG_FIRST(64, "c++ throw: %s", text.c_str());
    }
    uint32_t esp = ctx.esp;
    ctx.esp = (ctx.esp - kContextBytes) & ~15u;
    uint32_t scratch = ctx.esp;
    std::memset(HostPtr(scratch), 0, kContextBytes);
    for (uint32_t frame = R32(ctx.fs_base); frame && frame != 0xFFFFFFFF; frame = R32(frame)) {
        if (CallHandler(ctx, rec, frame, scratch) == kContinueExecution) {
            ctx.esp = esp;
            return 0;
        }
    }
    ctx.esp = esp;
    LOGE("RtlRaiseException code %08X flags %X address %08X params %u, not handled", R32(rec), R32(rec + 4), R32(rec + 12),
         R32(rec + 16));
    for (uint32_t i = 0; i < std::min<uint32_t>(R32(rec + 16), 4); i++) LOGE("  param %u = %08X", i, R32(rec + 20 + i * 4));
    if (!text.empty()) LOGE("  thrown: %s", text.c_str());
    x86_trap(ctx, R32(ctx.esp), "guest raised an exception");
}

// RtlUnwind(target frame, target ip, record, return value): the handlers of the frames above the target run with
// the unwinding flag (destructors), each frame leaving the chain after its handler
KERNEL_EXPORT(312, RtlUnwind, 4) {
    uint32_t target = a[0], rec = a[2];
    uint32_t esp = ctx.esp;
    ctx.esp = (ctx.esp - kContextBytes - 0x60) & ~15u;
    uint32_t scratch = ctx.esp;
    std::memset(HostPtr(scratch), 0, kContextBytes + 0x60);
    if (!rec) {
        rec = scratch + kContextBytes;
        W32(rec, 0xC0000027);  // STATUS_UNWIND
    }
    W32(rec + 4, R32(rec + 4) | kUnwinding | (target ? 0 : kExitUnwind));
    for (uint32_t frame = R32(ctx.fs_base); frame && frame != 0xFFFFFFFF && frame != target; frame = R32(ctx.fs_base)) {
        CallHandler(ctx, rec, frame, scratch);
        W32(ctx.fs_base, R32(frame));
    }
    ctx.esp = esp;
    return a[3];
}

static std::string FormatGuest(X86Context& ctx, uint32_t fmt_va, uint32_t args_va) {
    std::string fmt = ReadCString(fmt_va), out;
    uint32_t ap = args_va;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') {
            out += fmt[i];
            continue;
        }
        std::string spec = "%";
        i++;
        while (i < fmt.size() && std::strchr("-+ #0123456789.lhI", fmt[i])) spec += fmt[i++];
        if (i >= fmt.size()) break;
        char c = fmt[i];
        char buf[512];
        if (c == '%') {
            out += '%';
            continue;
        }
        if (c == 's') {
            out += ReadCString(R32(ap), 256);
            ap += 4;
            continue;
        }
        if (c == 'f' || c == 'g' || c == 'e') {
            double d;
            std::memcpy(&d, HostPtr(ap), 8);
            ap += 8;
            std::snprintf(buf, sizeof(buf), (spec + c).c_str(), d);
            out += buf;
            continue;
        }
        std::string clean;
        for (char ch : spec)
            if (ch != 'l' && ch != 'h' && ch != 'I') clean += ch;
        std::snprintf(buf, sizeof(buf), (clean + (c == 'p' ? 'x' : c)).c_str(), R32(ap));
        ap += 4;
        out += buf;
    }
    (void)ctx;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

static void DbgPrintEntry(X86Context& ctx, uint8_t*) {
    LOGI("DbgPrint: %s", FormatGuest(ctx, R32(ctx.esp + 4), ctx.esp + 8).c_str());
    ctx.eax = 0;
    ctx.esp += 4;  // cdecl
}
static const bool kreg_DbgPrint = (RegisterExport(8, "DbgPrint", DbgPrintEntry), true);

KERNEL_EXPORT(5, DbgBreakPoint, 0) {
    LOGW("DbgBreakPoint from %08X", R32(ctx.esp));
    return 0;
}

KERNEL_EXPORT(95, KeBugCheck, 1) {
    LOGE("KeBugCheck %08X", a[0]);
    x86_trap(ctx, R32(ctx.esp), "KeBugCheck");
}

KERNEL_EXPORT(49, HalReturnToFirmware, 1) {
    LOGI("HalReturnToFirmware(%u): the title asked to leave", a[0]);
    // quick reboot into a recompiled image, the XAPI wrote the launch data page: restart into its executable with it
    uint32_t page = R32(g_launch_data_page_va);
    if (a[0] == 2 && page) {
        std::string path = ReadCString(page + 8, 520);
        if (const char* exe = ImageExe(path)) {
            if (FILE* f = std::fopen(g_launch_file.c_str(), "wb")) {
                std::fwrite(HostPtr(page), 1, 0x1000, f);
                std::fclose(f);
            }
            LOGI("relaunching into %s", path.c_str());
            Relaunch(exe);
        }
        LOGW("HalReturnToFirmware: %s is no recompiled image", path.c_str());
    }
    LogGuestStack(ctx, 256);
    Shutdown(0);
}

KERNEL_EXPORT(360, HalInitiateShutdown, 0) { Shutdown(0); }
KERNEL_EXPORT(358, HalIsResetOrShutdownPending, 0) { return 0; }
KERNEL_EXPORT(47, HalRegisterShutdownNotification, 2) { return 0; }
KERNEL_EXPORT(37, FscSetCacheSize, 1) { return kSuccess; }

// ---------------------------------------------------------------- settings and hardware

// widescreen (a 4:3 output plays the console's 4:3), 480p
static uint32_t VideoFlags() { return (app::Widescreen() ? 0x00010000 : 0) | 0x00080000; }

KERNEL_EXPORT(24, ExQueryNonVolatileSetting, 5) {
    uint32_t index = a[0], type_out = a[1], value = a[2], len = a[3], result_len = a[4];
    uint32_t v = 0;
    switch (index) {
        case 7: v = 1; break;                      // XC_LANGUAGE english
        case 8: v = VideoFlags(); break;          // XC_VIDEO_FLAGS
        case 9: v = 0x00010001; break;             // XC_AUDIO_FLAGS stereo
        case 0x103: v = 0x00400100; break;         // XC_FACTORY_AV_REGION NTSC-M
        case 0x104: v = 1; break;                  // XC_FACTORY_GAME_REGION north america
        case 0x12: v = 1; break;                   // XC_DVD_REGION
        default: break;
    }
    if (value && len) std::memset(HostPtr(value), 0, len);
    if (index == 0xFFFF) {
        // the whole EEPROM image: user settings live at 0xC0 onwards
        if (type_out) W32(type_out, 3);  // REG_BINARY
        if (value && len >= 0x100) {
            W32(value + 0xE8, 1);                     // language
            W32(value + 0xEC, VideoFlags());          // video flags
            W32(value + 0xF0, 0x00010001);            // audio flags
            W32(value + 0x58, 0x00400100);            // factory video standard
            W32(value + 0x2C, 1);                     // factory game region
        }
        if (result_len) W32(result_len, 0x100);
        return kSuccess;
    }
    if (type_out) W32(type_out, 4);  // REG_DWORD
    if (value && len >= 4) W32(value, v);
    if (result_len) W32(result_len, 4);
    LOG_FIRST(32, "ExQueryNonVolatileSetting %X -> %X", index, v);
    return kSuccess;
}

KERNEL_EXPORT(1, AvGetSavedDataAddress, 0) { return SavedDataAddress(); }
KERNEL_EXPORT(4, AvSetSavedDataAddress, 1) {
    SetSavedDataAddress(a[0]);
    return 0;
}
KERNEL_EXPORT(3, AvSetDisplayMode, 6) { return 0; }
KERNEL_EXPORT(2, AvSendTVEncoderOption, 4) {
    // AV_QUERY_AV_CAPABILITIES: component (HDTV) pack, NTSC-M, 60 Hz
    if (a[3]) W32(a[3], a[1] == 6 ? 0x00400104 : 0);
    return 0;
}
KERNEL_EXPORT(46, HalReadWritePCISpace, 6) {
    if (!(a[5] & 0xFF) && a[3]) std::memset(HostPtr(a[3]), 0, a[4]);
    return 0;
}
KERNEL_EXPORT(252, PhyGetLinkState, 1) { return 0; }
KERNEL_EXPORT(253, PhyInitialize, 2) { return kSuccess; }

// ---------------------------------------------------------------- I/O manager, not used by the replaced libraries

KERNEL_EXPORT(65, IoCreateDevice, 6) { return kUnsuccessful; }
KERNEL_EXPORT(74, IoInvalidDeviceRequest, 2) { return 0xC0000010; }
KERNEL_EXPORT(62, IoBuildSynchronousFsdRequest, 7) { return 0; }
KERNEL_EXPORT(85, IoSynchronousFsdRequest, 5) { return kUnsuccessful; }
KERNEL_EXPORT(84, IoSynchronousDeviceIoControlRequest, 8) {
    if (a[4] && a[5]) std::memset(HostPtr(a[4]), 0, a[5]);
    if (a[6]) W32(a[6], 0);
    return kUnsuccessful;
}
KERNEL_EXPORT(83, IoStartPacket, 3) { return 0; }
KERNEL_EXPORT(81, IoStartNextPacket, 1) { return 0; }
KERNEL_EXPORT(359, IoMarkIrpMustComplete, 1) { return 0; }
KERNEL_FASTCALL(86, IofCallDriver) { return kUnsuccessful; }
KERNEL_FASTCALL(87, IofCompleteRequest) { return 0; }

// ---------------------------------------------------------------- crypto

KERNEL_EXPORT(335, XcSHAInit, 1) {
    crypto::ShaInit(HostPtr(a[0]));
    return 0;
}
KERNEL_EXPORT(336, XcSHAUpdate, 3) {
    crypto::ShaUpdate(HostPtr(a[0]), static_cast<const uint8_t*>(HostPtr(a[1])), a[2]);
    return 0;
}
KERNEL_EXPORT(337, XcSHAFinal, 2) {
    crypto::ShaFinal(HostPtr(a[0]), static_cast<uint8_t*>(HostPtr(a[1])));
    return 0;
}
KERNEL_EXPORT(340, XcHMAC, 7) {
    crypto::Hmac(static_cast<const uint8_t*>(HostPtr(a[0])), a[1], a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr,
                 a[3], a[4] ? static_cast<const uint8_t*>(HostPtr(a[4])) : nullptr, a[5], static_cast<uint8_t*>(HostPtr(a[6])));
    return 0;
}
KERNEL_EXPORT(338, XcRC4Key, 3) {
    crypto::Rc4Key(HostPtr(a[0]), static_cast<const uint8_t*>(HostPtr(a[2])), a[1]);
    return 0;
}
KERNEL_EXPORT(339, XcRC4Crypt, 3) {
    crypto::Rc4Crypt(HostPtr(a[0]), static_cast<uint8_t*>(HostPtr(a[2])), a[1]);
    return 0;
}
KERNEL_EXPORT(346, XcDESKeyParity, 2) { return 0; }
KERNEL_EXPORT(347, XcKeyTable, 3) { return 0; }
KERNEL_EXPORT(349, XcBlockCryptCBC, 7) {
    // only the online stack encrypts with DES; plain copy keeps buffers defined
    if (a[2] != a[3]) std::memmove(HostPtr(a[2]), HostPtr(a[3]), a[1]);
    return 0;
}
KERNEL_EXPORT(345, XcModExp, 5) { return 0; }
KERNEL_EXPORT(344, XcVerifyPKCS1Signature, 3) { return 1; }

}  // namespace kernel
