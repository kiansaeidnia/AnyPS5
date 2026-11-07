#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include <SDL_loadso.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Recorder;
using AgcDriver::Graphics::RuntimeSwitch;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;
namespace RuntimeSwitches = AgcDriver::Graphics::RuntimeSwitches;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("lane storage write-back: " + message);
}

constexpr const char* Reason = "written buffer range";
constexpr std::size_t BlockBytes = 1u << 20u;
constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 128;
constexpr std::size_t ImageBytes = Width * Height * 4u;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Initial = 0x3f800000u;
constexpr std::uint32_t Stored = 0x11111111u;
constexpr std::size_t FillDword = 0x100u / 4u;
constexpr std::size_t FarDword = 100u * Width;
constexpr std::uint32_t Filled = 0x33333333u;

alignas(256) constexpr std::array<std::uint32_t, 11> StoreImageCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, Stored, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "cannot allocate the guest block");
        std::fill(Words(), Words() + BlockBytes / 4u, Initial);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
        AgcDriver::GuestMemory::CollectWritesUncached(Address(), BlockBytes);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint32_t* Words() const { return reinterpret_cast<std::uint32_t*>(block); }
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
};

// A VkEvent the async lane's batch waits for on the GPU until Release().
class Gate {
public:
    explicit Gate(AgcDriver::VulkanDevice& device) : device(device) {
#ifdef _WIN32
        constexpr const char* libraryName = "vulkan-1.dll";
#else
        constexpr const char* libraryName = "libvulkan.so.1";
#endif
        library.reset(SDL_LoadObject(libraryName));
        Require(library != nullptr, "cannot load Vulkan for the gate");
        resolve = reinterpret_cast<PFN_vkGetDeviceProcAddr>(SDL_LoadFunction(library.get(), "vkGetDeviceProcAddr"));
        Require(resolve != nullptr, "cannot resolve vkGetDeviceProcAddr");
        destroyEvent = function<PFN_vkDestroyEvent>("vkDestroyEvent");
        setEvent = function<PFN_vkSetEvent>("vkSetEvent");
        waitEvents = function<PFN_vkCmdWaitEvents>("vkCmdWaitEvents");
        const VkEventCreateInfo info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        Require(function<PFN_vkCreateEvent>("vkCreateEvent")(device.Device(), &info, nullptr, &event) == VK_SUCCESS, "cannot create the gate event");
    }

    ~Gate() {
        setEvent(device.Device(), event);
        try {
            device.WaitIdle();
        } catch (...) {
        }
        destroyEvent(device.Device(), event, nullptr);
    }

    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;

    void Record(VkCommandBuffer commands) {
        waitEvents(commands, 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
    }

    void Release() {
        Require(setEvent(device.Device(), event) == VK_SUCCESS, "cannot release the gate");
    }

private:
    template <typename TFunction>
    TFunction function(const char* name) const {
        const auto found = reinterpret_cast<TFunction>(resolve(device.Device(), name));
        Require(found != nullptr, std::string("cannot resolve ") + name);
        return found;
    }

    AgcDriver::VulkanDevice& device;
    std::unique_ptr<void, decltype(&SDL_UnloadObject)> library{nullptr, &SDL_UnloadObject};
    PFN_vkGetDeviceProcAddr resolve = nullptr;
    PFN_vkDestroyEvent destroyEvent = nullptr;
    PFN_vkSetEvent setEvent = nullptr;
    PFN_vkCmdWaitEvents waitEvents = nullptr;
    VkEvent event = VK_NULL_HANDLE;
};

ShaderRecompiler::RecompileResult Compile(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const ShaderRecompiler::SpirvTarget& target) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64u, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

std::vector<std::uint32_t> ImageUserData(std::uint64_t base) {
    std::vector<std::uint32_t> userData(12, 0u);
    userData[4] = static_cast<std::uint32_t>(base >> 8u);
    userData[5] = static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Width - 1u) & 3u) << 30u);
    userData[6] = ((Width - 1u) >> 2u) | ((Height - 1u) << 14u);
    userData[7] = 0xfacu | (Type2D << 28u);
    return userData;
}

// The image's results stay pending on the main lane; a fill inside its pending layer is recorded on the async lane
// behind the gate, so it cannot finish before Release(). A GPU-ordered flush of the image over that fill must not
// wait for it on the CPU (unless APS5_NO_DEFER_LANE_EDGE), and once the gate opens the fill's bytes and the image's
// other texels must both be in guest memory.
void Run(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset, bool edge) {
    const std::string what = std::string(edge ? "lane edge" : "APS5_NO_DEFER_LANE_EDGE") + ": ";
    RuntimeSwitches::Assign(RuntimeSwitch::NoDeferLaneEdge, !edge);
    const auto* words = guest.Words() + offset / 4u;
    const auto image = guest.Address() + offset;
    const auto storeImage = Compile(StoreImageCode, ImageUserData(image), device.Target());
    device.Dispatch(storeImage, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreImageCode.data()));
    Gate gate(device);
    const auto fill = image + FillDword * 4u;
    {
        const Recorder::LaneScope async(1);
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            gate.Record(Recorder::Active()->Commands());
        }
        const std::array<std::uint32_t, 4> pattern{Filled, Filled, Filled, Filled};
        Require(device.FillBuffer(fill, 16u, pattern), what + "the fill was not recorded on the GPU");
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Require(Recorder::Active()->LaneWriterOverlaps(1, fill, 16u), what + "the fill is not a writer of the async lane");
    }
    const auto before = StorageTexture::GpuOrderTotals();
    std::atomic<bool> finished{false};
    std::exception_ptr failure;
    bool stored = false;
    std::thread flusher([&] {
        try {
            stored = StorageTexture::FlushPending(image, ImageBytes, nullptr, Reason);
        } catch (...) {
            failure = std::current_exception();
        }
        finished.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(edge ? 10 : 1);
    while (!finished.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool returnedWhileHeld = finished.load();
    gate.Release();
    flusher.join();
    if (failure) std::rethrow_exception(failure);
    device.WaitIdle();
    const auto after = StorageTexture::GpuOrderTotals();
    Require(stored, what + "the image store left no results pending");
    if (edge) {
        Require(returnedWhileHeld, what + "the flush waited on the CPU for the async lane's writer");
        Require(after.laneEdges - before.laneEdges == 1u, what + "the flush did not order the main lane after the async lane");
        Require(after.lane == before.lane, what + "the flush was kept waiting by the async-lane condition");
        Require(after.deferred - before.deferred == 1u, what + "the flush was not GPU-ordered");
    } else {
        Require(!returnedWhileHeld, what + "the flush returned while the async lane's writer could not finish");
        Require(after.lane - before.lane == 1u, what + "the async-lane condition did not keep the flush waiting");
        Require(after.laneEdges == before.laneEdges, what + "the switch did not turn the lane edge off");
    }
    Require(words[FillDword] == Filled, what + "the pending image's store put its older texels over the async lane's newer fill");
    Require(words[FarDword] == Stored, what + "the pending image's texel away from the fill was not stored");
    RuntimeSwitches::Assign(RuntimeSwitch::NoDeferLaneEdge, false);
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_ASYNC_LANE", "51");
#else
    setenv("APS5_ASYNC_LANE", "51", 1);
#endif
    try {
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        bool active = false;
        {
            std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
            active = Recorder::Active() != nullptr && Recorder::Active()->AsyncLaneActive();
        }
        const bool watched = AgcDriver::GuestMemory::WriteWatched();
        if (!active || !watched || RuntimeSwitches::Set(RuntimeSwitch::NoDeferStores)) {
            std::puts(!active ? "skipped: the device has no third queue for the async lane" : !watched ? "skipped: guest memory has no write watch" : "skipped: APS5_NO_DEFER_STORES is set");
            device.reset();
            AgcDriverShutdown_nid_postfix();
            return VulkanTestSkipped;
        }
        {
            GuestBlock guest;
            Run(*device, guest, 0, true);
            Run(*device, guest, BlockBytes / 2u, false);
        }
        device.reset();
        std::puts("lane storage write-back tests passed");
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}
