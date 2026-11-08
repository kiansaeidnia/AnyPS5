#include "prx/libSceAgcDriver/Execution/include/ReplayMetrics.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace AgcDriver {

namespace {

using Clock = std::chrono::steady_clock;

struct State {
    std::mutex mutex;
    std::array<std::uint64_t, 2> retiredCpuNs{};
    std::map<const void*, std::pair<ReplayThreadRole, std::function<std::uint64_t()>>> threads;
    std::map<std::uint32_t, std::uint64_t> waits;
    ReplayWaitMetrics* activeWait = nullptr;
    std::map<double, double> gpuRanges;
    double gpuBusyNs = 0;
    double gpuBatchNs = 0;
    std::uint64_t gpuBatches = 0;
    std::exception_ptr failure;
};

State& Metrics() {
    static State state;
    return state;
}

std::atomic<bool> enabled{false};

std::uint64_t ElapsedNs(Clock::time_point begin, Clock::time_point end) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

}

struct ReplayThreadMetrics::State {
#ifdef _WIN32
    HANDLE handle = nullptr;

    State() {
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, THREAD_QUERY_INFORMATION, FALSE, 0))
            throw std::system_error(GetLastError(), std::system_category(), "Opening replay CPU clock");
    }

    ~State() { CloseHandle(handle); }

    std::uint64_t CpuNs() const {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetThreadTimes(handle, &created, &exited, &kernel, &user))
            throw std::system_error(GetLastError(), std::system_category(), "Reading replay CPU clock");
        const auto ticks = [](const FILETIME& time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
        return (ticks(kernel) + ticks(user)) * 100;
    }
#elif defined(__APPLE__)
    thread_t thread = mach_thread_self();

    ~State() { mach_port_deallocate(mach_task_self(), thread); }

    std::uint64_t CpuNs() const {
        thread_basic_info_data_t info{};
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
        const auto result = thread_info(thread, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count);
        if (result != KERN_SUCCESS) throw std::runtime_error("Reading replay CPU clock failed: " + std::to_string(result));
        return (static_cast<std::uint64_t>(info.user_time.seconds) + info.system_time.seconds) * 1000000000
            + (static_cast<std::uint64_t>(info.user_time.microseconds) + info.system_time.microseconds) * 1000;
    }
#else
    clockid_t clock{};

    State() {
        const auto result = pthread_getcpuclockid(pthread_self(), &clock);
        if (result != 0) throw std::system_error(result, std::generic_category(), "Opening replay CPU clock");
    }

    std::uint64_t CpuNs() const {
        timespec value{};
        if (clock_gettime(clock, &value) != 0) throw std::system_error(errno, std::generic_category(), "Reading replay CPU clock");
        return static_cast<std::uint64_t>(value.tv_sec) * 1000000000 + static_cast<std::uint64_t>(value.tv_nsec);
    }
#endif
    ReplayThreadRole role = ReplayThreadRole::QueueZero;
    std::uint64_t initialNs = 0;
};

ReplayThreadMetrics::ReplayThreadMetrics(ReplayThreadRole role, bool active) {
    if (!active || !ReplayMetrics::Enabled()) return;
    auto& metrics = Metrics();
    std::lock_guard lock(metrics.mutex);
    try {
        state = std::make_unique<State>();
        state->role = role;
        state->initialNs = state->CpuNs();
        metrics.threads.emplace(state.get(), std::make_pair(role, [thread = state.get()] { return thread->CpuNs() - thread->initialNs; }));
    } catch (...) {
        state.reset();
        metrics.failure = std::current_exception();
    }
}

ReplayThreadMetrics::~ReplayThreadMetrics() {
    if (!state) return;
    auto& metrics = Metrics();
    std::lock_guard lock(metrics.mutex);
    try {
        metrics.retiredCpuNs[static_cast<std::size_t>(state->role)] += state->CpuNs() - state->initialNs;
    } catch (...) {
        metrics.failure = std::current_exception();
    }
    metrics.threads.erase(state.get());
}

ReplayWaitMetrics::ReplayWaitMetrics(std::uint32_t queue) : queue(queue) {
    if (!ReplayMetrics::Enabled()) return;
    begun = Clock::now();
    auto& metrics = Metrics();
    std::lock_guard lock(metrics.mutex);
    metrics.waits.try_emplace(queue, 0);
    next = metrics.activeWait;
    if (next) next->previous = this;
    metrics.activeWait = this;
    active = true;
}

ReplayWaitMetrics::~ReplayWaitMetrics() {
    if (!active) return;
    auto& metrics = Metrics();
    std::lock_guard lock(metrics.mutex);
    const auto end = Clock::now();
    metrics.waits.at(queue) += ElapsedNs(begun, end);
    if (previous) previous->next = next;
    else metrics.activeWait = next;
    if (next) next->previous = previous;
}

bool ReplayMetrics::Enabled() {
    return enabled.load(std::memory_order_relaxed);
}

void ReplayMetrics::Enable() {
    enabled.store(true, std::memory_order_relaxed);
}

ReplayMetricsSnapshot ReplayMetrics::Snapshot() {
    if (!Enabled()) throw std::runtime_error("Replay metrics have not been enabled");
    auto& metrics = Metrics();
    std::lock_guard lock(metrics.mutex);
    if (metrics.failure) std::rethrow_exception(metrics.failure);
    auto cpu = metrics.retiredCpuNs;
    for (const auto& [thread, reader] : metrics.threads) cpu[static_cast<std::size_t>(reader.first)] += reader.second();
    auto waits = metrics.waits;
    const auto now = Clock::now();
    for (auto* wait = metrics.activeWait; wait; wait = wait->next) waits.at(wait->queue) += ElapsedNs(wait->begun, now);
    ReplayMetricsSnapshot result;
    result.queue0CpuNs = cpu[static_cast<std::size_t>(ReplayThreadRole::QueueZero)];
    result.drawCommitterCpuNs = cpu[static_cast<std::size_t>(ReplayThreadRole::DrawCommitter)];
    result.gpuBusyNs = metrics.gpuBusyNs;
    result.gpuBatchNs = metrics.gpuBatchNs;
    result.gpuBatches = metrics.gpuBatches;
    for (const auto& [queue, ns] : waits) result.queues.push_back({queue, ns});
    return result;
}

void ReplayMetrics::GpuRange(double beginNs, double endNs) {
    if (!Enabled()) return;
    if (!std::isfinite(beginNs) || !std::isfinite(endNs) || endNs < beginNs)
        throw std::runtime_error("Invalid replay GPU timestamp range");
    auto& metrics = Metrics();
    std::lock_guard lock(metrics.mutex);
    metrics.gpuBatchNs += endNs - beginNs;
    ++metrics.gpuBatches;
    auto next = metrics.gpuRanges.lower_bound(beginNs);
    if (next != metrics.gpuRanges.begin() && std::prev(next)->second >= beginNs) --next;
    while (next != metrics.gpuRanges.end() && next->first <= endNs) {
        beginNs = std::min(beginNs, next->first);
        endNs = std::max(endNs, next->second);
        metrics.gpuBusyNs -= next->second - next->first;
        next = metrics.gpuRanges.erase(next);
    }
    metrics.gpuRanges.emplace(beginNs, endNs);
    metrics.gpuBusyNs += endNs - beginNs;
}

}

extern "C" void AgcDriverReplayMetricsEnable_nid_no_patch() {
    AgcDriver::ReplayMetrics::Enable();
}

extern "C" AgcDriver::ReplayMetricsSnapshot AgcDriverReplayMetricsSnapshot_nid_no_patch() {
    return AgcDriver::ReplayMetrics::Snapshot();
}
