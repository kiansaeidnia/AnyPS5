#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_REPLAYMETRICS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_REPLAYMETRICS_HPP

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace AgcDriver {

struct ReplayQueueMetrics {
    std::uint32_t queue = 0;
    std::uint64_t waitRegMemNs = 0;
};

struct ReplayMetricsSnapshot {
    std::uint64_t queue0CpuNs = 0;
    std::uint64_t drawCommitterCpuNs = 0;
    double gpuBusyNs = 0;
    double gpuBatchNs = 0;
    std::uint64_t gpuBatches = 0;
    std::vector<ReplayQueueMetrics> queues;
};

enum class ReplayThreadRole { QueueZero, DrawCommitter };

class ReplayThreadMetrics {
public:
    explicit ReplayThreadMetrics(ReplayThreadRole role, bool active = true);
    ~ReplayThreadMetrics();
    ReplayThreadMetrics(const ReplayThreadMetrics&) = delete;
    ReplayThreadMetrics& operator=(const ReplayThreadMetrics&) = delete;

private:
    struct State;
    std::unique_ptr<State> state;
};

class ReplayWaitMetrics {
public:
    explicit ReplayWaitMetrics(std::uint32_t queue);
    ~ReplayWaitMetrics();
    ReplayWaitMetrics(const ReplayWaitMetrics&) = delete;
    ReplayWaitMetrics& operator=(const ReplayWaitMetrics&) = delete;

private:
    friend class ReplayMetrics;
    std::uint32_t queue;
    std::chrono::steady_clock::time_point begun{};
    ReplayWaitMetrics* previous = nullptr;
    ReplayWaitMetrics* next = nullptr;
    bool active = false;
};

class ReplayMetrics {
public:
    static bool Enabled();
    static void Enable();
    static ReplayMetricsSnapshot Snapshot();
    static void GpuRange(double beginNs, double endNs);
};

}

extern "C" void AgcDriverReplayMetricsEnable_nid_no_patch();
extern "C" AgcDriver::ReplayMetricsSnapshot AgcDriverReplayMetricsSnapshot_nid_no_patch();

#endif
