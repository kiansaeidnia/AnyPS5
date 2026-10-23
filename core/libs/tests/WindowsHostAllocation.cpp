#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <vector>
#include <windows.h>

namespace {

volatile std::size_t impossible = std::size_t{1} << 46;
unsigned handlerCalls;
unsigned handlerFailuresDestroyed;
unsigned guardsDestroyed;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Guard {
    ~Guard() { ++guardsDestroyed; }
};

struct HandlerScope {
    std::new_handler previous;
    explicit HandlerScope(std::new_handler handler) : previous(std::set_new_handler(handler)) {}
    ~HandlerScope() { std::set_new_handler(previous); }
};

struct HandlerFailure : std::bad_alloc {
    ~HandlerFailure() override { ++handlerFailuresDestroyed; }
};

void RetryHandler() {
    if (++handlerCalls == 3) std::set_new_handler(nullptr);
}

void ThrowHandler() {
    ++handlerCalls;
    throw HandlerFailure{};
}

struct Allocation {
    void* (*allocate)(std::size_t);
    void (*release)(void*);
    std::size_t alignment;
};

constexpr std::align_val_t Alignment{4096};

const std::array<Allocation, 4> Throwing{{
    {[](std::size_t size) { return ::operator new(size); }, [](void* block) { ::operator delete(block); }, alignof(std::max_align_t)},
    {[](std::size_t size) { return ::operator new[](size); }, [](void* block) { ::operator delete[](block); }, alignof(std::max_align_t)},
    {[](std::size_t size) { return ::operator new(size, Alignment); }, [](void* block) { ::operator delete(block, Alignment); }, 4096},
    {[](std::size_t size) { return ::operator new[](size, Alignment); }, [](void* block) { ::operator delete[](block, Alignment); }, 4096},
}};

const std::array<Allocation, 4> Nothrow{{
    {[](std::size_t size) { return ::operator new(size, std::nothrow); }, [](void* block) { ::operator delete(block, std::nothrow); }, alignof(std::max_align_t)},
    {[](std::size_t size) { return ::operator new[](size, std::nothrow); }, [](void* block) { ::operator delete[](block, std::nothrow); }, alignof(std::max_align_t)},
    {[](std::size_t size) { return ::operator new(size, Alignment, std::nothrow); }, [](void* block) { ::operator delete(block, Alignment, std::nothrow); }, 4096},
    {[](std::size_t size) { return ::operator new[](size, Alignment, std::nothrow); }, [](void* block) { ::operator delete[](block, Alignment, std::nothrow); }, 4096},
}};

void CheckSuccessfulAllocations() {
    for (const auto& forms : {Throwing, Nothrow}) {
        for (const auto& form : forms) {
            for (const auto size : {std::size_t{0}, std::size_t{1}, std::size_t{257}, std::size_t{8193}}) {
                void* block = form.allocate(size);
                Require(block != nullptr, "a small host allocation failed");
                Require(reinterpret_cast<std::uintptr_t>(block) % form.alignment == 0, "a host allocation has incorrect alignment");
                if (size != 0) std::memset(block, 0x5a, size);
                form.release(block);
            }
            form.release(nullptr);
        }
    }
    ::operator delete(::operator new(65), std::size_t{65});
    ::operator delete[](::operator new[](65), std::size_t{65});
    ::operator delete(::operator new(65, Alignment), std::size_t{65}, Alignment);
    ::operator delete[](::operator new[](65, Alignment), std::size_t{65}, Alignment);
}

void CheckFailures() {
    HandlerScope handlers(nullptr);
    for (const auto& form : Throwing) {
        guardsDestroyed = 0;
        bool caught = false;
        try {
            Guard guard;
            form.release(form.allocate(impossible));
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        Require(caught && guardsDestroyed == 1, "a host allocation failure did not unwind to bad_alloc");
    }
    for (const auto& form : Nothrow) Require(form.allocate(impossible) == nullptr, "a nothrow host allocation did not return null");
    bool caught = false;
    try {
        std::vector<std::byte> bytes;
        bytes.resize(impossible);
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    Require(caught, "a vector allocation failure was not caught");
}

void CheckHandlers() {
    for (const auto& form : Throwing) {
        handlerCalls = 0;
        bool caught = false;
        {
            HandlerScope handlers(RetryHandler);
            try {
                form.release(form.allocate(impossible));
            } catch (const std::bad_alloc&) {
                caught = true;
            }
        }
        Require(caught && handlerCalls == 3, "a returning new_handler was not retried");
        handlerCalls = 0;
        handlerFailuresDestroyed = 0;
        caught = false;
        {
            HandlerScope handlers(ThrowHandler);
            try {
                form.release(form.allocate(impossible));
            } catch (const HandlerFailure&) {
                caught = true;
            }
        }
        Require(caught && handlerCalls == 1 && handlerFailuresDestroyed == 1, "a new_handler exception lost its type or lifetime");
    }
    for (const auto& form : Nothrow) {
        handlerCalls = 0;
        handlerFailuresDestroyed = 0;
        HandlerScope handlers(ThrowHandler);
        Require(form.allocate(impossible) == nullptr, "a nothrow allocation leaked a new_handler exception");
        Require(handlerCalls == 1 && handlerFailuresDestroyed == 1, "a nothrow allocation lost its new_handler exception");
    }
}

template<class Function>
Function HostFunction(const char* name) {
    const HMODULE module = GetModuleHandleW(L"libstdc++-6.dll");
    Require(module != nullptr, "the host C++ runtime was not loaded");
    const auto address = GetProcAddress(module, name);
    Require(address != nullptr, "a host allocation function was not exported");
    return reinterpret_cast<Function>(reinterpret_cast<void*>(address));
}

void CheckHostOwnership() {
    const auto hostNew = HostFunction<void* (*)(std::size_t)>("_Znwy");
    const auto hostDelete = HostFunction<void (*)(void*)>("_ZdlPv");
    const auto hostAlignedNew = HostFunction<void* (*)(std::size_t, std::align_val_t)>("_ZnwySt11align_val_t");
    const auto hostAlignedDelete = HostFunction<void (*)(void*, std::align_val_t)>("_ZdlPvSt11align_val_t");
    for (const auto size : {std::size_t{1}, std::size_t{257}, std::size_t{8193}}) {
        ::operator delete(hostNew(size));
        hostDelete(::operator new(size));
        ::operator delete(hostAlignedNew(size, Alignment), Alignment);
        hostAlignedDelete(::operator new(size, Alignment), Alignment);
    }
}

}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        CheckSuccessfulAllocations();
        CheckHostOwnership();
        CheckFailures();
        CheckHandlers();
        std::puts("host allocation failures, handlers, alignment and ownership passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
