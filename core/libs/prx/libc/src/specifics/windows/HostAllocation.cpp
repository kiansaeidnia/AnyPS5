#ifdef _WIN32

#include <bit>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <malloc.h>
#include <new>

#include "prx/libc/include/general/ExportMacros.hpp"

APS5_EXPORT("_Znwy", _Znwy);
APS5_EXPORT("_Znay", _Znay);
APS5_EXPORT("_ZnwySt11align_val_t", _ZnwySt11align_val_t);
APS5_EXPORT("_ZnaySt11align_val_t", _ZnaySt11align_val_t);
APS5_EXPORT("_ZnwyRKSt9nothrow_t", _ZnwyRKSt9nothrow_t);
APS5_EXPORT("_ZnayRKSt9nothrow_t", _ZnayRKSt9nothrow_t);
APS5_EXPORT("_ZnwySt11align_val_tRKSt9nothrow_t", _ZnwySt11align_val_tRKSt9nothrow_t);
APS5_EXPORT("_ZnaySt11align_val_tRKSt9nothrow_t", _ZnaySt11align_val_tRKSt9nothrow_t);

namespace {

void* Allocate(std::size_t bytes, std::size_t alignment = 0) {
    if (bytes == 0) bytes = 1;
    for (;;) {
        void* block = nullptr;
        if (alignment == 0) {
            block = std::malloc(bytes);
        } else if (alignment <= std::numeric_limits<std::size_t>::max() - sizeof(void*) &&
                   bytes <= std::numeric_limits<std::size_t>::max() - alignment - sizeof(void*)) {
            block = _aligned_malloc(bytes, alignment);
        }
        if (block != nullptr) return block;
        const auto handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

void* AllocateAligned(std::size_t bytes, std::align_val_t alignment) {
    const auto value = static_cast<std::size_t>(alignment);
    if (!std::has_single_bit(value)) throw std::bad_alloc();
    return Allocate(bytes, value);
}

}

void* operator new(std::size_t bytes) { return Allocate(bytes); }
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void* operator new(std::size_t bytes, std::align_val_t alignment) { return AllocateAligned(bytes, alignment); }
void* operator new[](std::size_t bytes, std::align_val_t alignment) { return ::operator new(bytes, alignment); }

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(bytes);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](bytes);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}

#endif
