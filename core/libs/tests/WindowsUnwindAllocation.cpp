#include "prx/libc/include/exceptions/Runtime.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <windows.h>

extern "C" {
void* APS5_VABI __cxa_get_globals_nid_postfix();
_Unwind_Reason_Code APS5_VABI _Unwind_Backtrace_nid_postfix(_Unwind_Trace_Fn, void*);
_Unwind_Ptr APS5_VABI _Unwind_GetRegionStart_nid_postfix(_Unwind_Context*);
void APS5_VABI UnwindAllocationFrame(void (APS5_VABI *)(void*), void*);
}

namespace {
std::atomic<bool> denying;
std::atomic<unsigned> deniedHeap;
std::atomic<unsigned> deniedVirtual;
std::atomic<unsigned> ready;
bool failHeap;
bool failVirtual;
unsigned workerCount;
HANDLE readyEvent;
HANDLE startEvent;
void* (*originalMalloc)(std::size_t);
decltype(&VirtualAlloc) originalVirtualAlloc;

struct Probe {
    void* object{};
    bool backtrace{};
    bool found{};
    unsigned cleaned{};
    unsigned destroyed{};
};

struct Payload {
    Probe* probe;
    int value;
};

void* FailMalloc(std::size_t bytes) {
    if (denying.load() && failHeap) {
        ++deniedHeap;
        return nullptr;
    }
    return originalMalloc(bytes);
}

LPVOID WINAPI FailVirtualAlloc(LPVOID address, SIZE_T size, DWORD allocation, DWORD protection) {
    if (denying.load() && failVirtual) {
        ++deniedVirtual;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return originalVirtualAlloc(address, size, allocation, protection);
}

bool InstallHooks(HMODULE module) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* imports = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; imports->Name; ++imports) {
        if (!imports->OriginalFirstThunk) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imports->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* symbol = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            void* replacement = nullptr;
            if (std::strcmp(symbol->Name, "malloc") == 0) {
                if (originalMalloc && reinterpret_cast<void*>(originalMalloc) != reinterpret_cast<void*>(slots->u1.Function)) return false;
                originalMalloc = reinterpret_cast<decltype(originalMalloc)>(slots->u1.Function);
                replacement = reinterpret_cast<void*>(FailMalloc);
            } else if (std::strcmp(symbol->Name, "VirtualAlloc") == 0) {
                originalVirtualAlloc = reinterpret_cast<decltype(originalVirtualAlloc)>(slots->u1.Function);
                replacement = reinterpret_cast<void*>(FailVirtualAlloc);
            }
            if (!replacement) continue;
            DWORD previous;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &previous)) return false;
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&slots->u1.Function), replacement);
            DWORD ignored;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), previous, &ignored)) return false;
        }
    }
    return true;
}

void APS5_VABI DestroyPayload(void* object) {
    auto* payload = static_cast<Payload*>(object);
    ++payload->probe->destroyed;
    payload->~Payload();
}

struct Guard {
    Probe& probe;
    ~Guard() { ++probe.cleaned; }
};

[[gnu::noinline]] void APS5_VABI ThrowPayload(void* argument) {
    auto& probe = *static_cast<Probe*>(argument);
    Guard guard{probe};
    __cxa_throw_nid_postfix(probe.object, const_cast<std::type_info*>(&typeid(Payload)),
        reinterpret_cast<void (*)(void*)>(DestroyPayload));
}

_Unwind_Reason_Code APS5_VABI Trace(_Unwind_Context* context, void* argument) {
    auto& probe = *static_cast<Probe*>(argument);
    if (_Unwind_GetRegionStart_nid_postfix(context) != reinterpret_cast<_Unwind_Ptr>(&UnwindAllocationFrame))
        return _URC_NO_REASON;
    probe.found = true;
    return _URC_NORMAL_STOP;
}

[[gnu::noinline]] void APS5_VABI Backtrace(void* argument) {
    if (_Unwind_Backtrace_nid_postfix(Trace, argument) != _URC_NORMAL_STOP)
        static_cast<Probe*>(argument)->found = false;
}

DWORD WINAPI Worker(void* argument) {
    auto& probe = *static_cast<Probe*>(argument);
    __cxa_get_globals_nid_postfix();
    if (++ready == workerCount) SetEvent(readyEvent);
    if (WaitForSingleObject(startEvent, 10000) != WAIT_OBJECT_0) return 2;
    if (probe.backtrace) {
        UnwindAllocationFrame(Backtrace, &probe);
        return probe.found ? 0 : 1;
    }
    bool caught = false;
    try {
        UnwindAllocationFrame(ThrowPayload, &probe);
    } catch (const Payload& payload) {
        caught = payload.probe == &probe && payload.value == 73;
    } catch (...) {}
    return caught && probe.cleaned == 1 && probe.destroyed == 1 ? 0 : 1;
}
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 2) return 2;
    const bool backtrace = std::strncmp(argv[1], "backtrace-", 10) == 0;
    const bool parallel = std::strncmp(argv[1], "parallel-", 9) == 0;
    const bool throwing = std::strncmp(argv[1], "throw-", 6) == 0;
    const auto* fault = std::strchr(argv[1], '-');
    if ((!backtrace && !parallel && !throwing) || !fault) return 2;
    failHeap = std::strcmp(fault, "-heap") == 0 || std::strcmp(fault, "-both") == 0;
    failVirtual = std::strcmp(fault, "-virtual") == 0 || std::strcmp(fault, "-both") == 0;
    if ((!failHeap && !failVirtual) || !InstallHooks(GetModuleHandleW(L"libc.prx")) ||
        !InstallHooks(GetModuleHandleW(L"libstdc++-6.dll")) || !originalMalloc || !originalVirtualAlloc) return 3;
    workerCount = parallel ? 8 : 1;
    readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    startEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent || !startEvent) return 4;
    Probe probes[8]{};
    HANDLE threads[8]{};
    for (unsigned i = 0; i < workerCount; ++i) {
        probes[i].backtrace = backtrace;
        if (!backtrace) {
            probes[i].object = __cxa_allocate_exception_nid_postfix(sizeof(Payload));
            new (probes[i].object) Payload{&probes[i], 73};
        }
        threads[i] = CreateThread(nullptr, 0, Worker, &probes[i], 0, nullptr);
        if (!threads[i]) return 5;
    }
    if (WaitForSingleObject(readyEvent, 10000) != WAIT_OBJECT_0) return 6;
    denying = true;
    if (failHeap && FailMalloc(16) != nullptr) return 7;
    SetEvent(startEvent);
    bool passed = WaitForMultipleObjects(workerCount, threads, TRUE, 10000) == WAIT_OBJECT_0;
    denying = false;
    for (unsigned i = 0; i < workerCount; ++i) {
        DWORD code = 1;
        passed = GetExitCodeThread(threads[i], &code) && code == 0 && passed;
        CloseHandle(threads[i]);
    }
    CloseHandle(readyEvent);
    CloseHandle(startEvent);
    std::printf("mode=%s passed=%d heap_denied=%u virtual_denied=%u workers=%u\n",
        argv[1], passed, deniedHeap.load(), deniedVirtual.load(), workerCount);
    std::fflush(nullptr);
    if (!passed) TerminateProcess(GetCurrentProcess(), 1);
    return passed ? 0 : 1;
}
