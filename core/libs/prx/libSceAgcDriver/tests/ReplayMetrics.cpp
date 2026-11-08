#include "prx/libSceAgcDriver/Execution/include/ReplayMetrics.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using AgcDriver::ReplayMetrics;

void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void GpuUnion() {
    const auto before = ReplayMetrics::Snapshot();
    ReplayMetrics::GpuRange(300, 400);
    ReplayMetrics::GpuRange(100, 200);
    ReplayMetrics::GpuRange(150, 350);
    ReplayMetrics::GpuRange(110, 120);
    const auto after = ReplayMetrics::Snapshot();
    Check(after.gpuBusyNs - before.gpuBusyNs == 300, "GPU busy time double-counted overlapping lanes");
    Check(after.gpuBatchNs - before.gpuBatchNs == 410, "GPU batch time missed a range");
    Check(after.gpuBatches - before.gpuBatches == 4, "GPU batch count missed a range");
}

std::uint64_t WaitNs(std::uint32_t queue) {
    for (const auto& value : ReplayMetrics::Snapshot().queues) if (value.queue == queue) return value.waitRegMemNs;
    return 0;
}

void ActiveWaits() {
    const auto before = WaitNs(17);
    std::uint64_t active = 0;
    {
        AgcDriver::ReplayWaitMetrics wait(17);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        active = WaitNs(17);
        Check(active - before >= 1000000, "An ongoing queue wait was omitted from its snapshot");
    }
    Check(WaitNs(17) >= active, "Completed queue wait lost its active interval");
    Check(WaitNs(18) == 0, "Queue wait leaked into another queue");
}

void CpuTime() {
    const auto before = ReplayMetrics::Snapshot();
    std::atomic<std::uint64_t> operations{0};
    std::thread worker([&] {
        AgcDriver::ReplayThreadMetrics metrics(AgcDriver::ReplayThreadRole::QueueZero);
        const auto until = Clock::now() + std::chrono::milliseconds(120);
        while (Clock::now() < until) operations.fetch_add(1, std::memory_order_relaxed);
    });
    worker.join();
    const auto busy = ReplayMetrics::Snapshot();
    Check(operations != 0 && busy.queue0CpuNs > before.queue0CpuNs, "Retired worker CPU time was lost");
    const auto cpu = busy.queue0CpuNs - before.queue0CpuNs;
    std::thread sleeper([] {
        AgcDriver::ReplayThreadMetrics metrics(AgcDriver::ReplayThreadRole::DrawCommitter);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    });
    sleeper.join();
    const auto after = ReplayMetrics::Snapshot();
    Check(after.drawCommitterCpuNs - before.drawCommitterCpuNs < cpu, "CPU time includes thread sleep");
    Check(after.queue0CpuNs == busy.queue0CpuNs, "Retired worker CPU time changed after its thread exited");
}

}

int main() {
    try {
        Check(!ReplayMetrics::Enabled(), "Replay metrics are enabled by default");
        AgcDriverReplayMetricsEnable_nid_no_patch();
        GpuUnion();
        ActiveWaits();
        CpuTime();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
