#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "prx/libc/include/general/AtomicSharedPtr.hpp"

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <class T>
bool Equivalent(const std::shared_ptr<T>& left, const std::shared_ptr<T>& right) {
    return left.get() == right.get() && !left.owner_before(right) && !right.owner_before(left);
}

void Compare(std::shared_ptr<int> current, std::shared_ptr<int> expected, bool success) {
    const auto previousExpected = expected;
    const auto desired = std::make_shared<int>(42);
    AtomicSharedPtr<int> value(current);
    Require(value.compare_exchange_strong(expected, desired, std::memory_order_acq_rel) == success,
            "compare_exchange_strong compared the pointer without its ownership");
    Require(Equivalent(value.load(std::memory_order_acquire), success ? desired : current),
            "compare_exchange_strong stored the wrong shared pointer");
    Require(Equivalent(expected, success ? previousExpected : current),
            "compare_exchange_strong returned the wrong expected shared pointer");
}

void Ownership() {
    int object = 0;
    auto current = std::shared_ptr<int>(&object, [](int*) {});
    Compare(current, std::shared_ptr<int>(&object, [](int*) {}), false);
    Compare(current, current, true);
    auto nullOwner = std::shared_ptr<int>(nullptr, [](int*) {});
    Compare(nullOwner, {}, false);
    Compare({}, nullOwner, false);
    Compare(nullOwner, std::shared_ptr<int>(nullptr, [](int*) {}), false);
    Compare(nullOwner, nullOwner, true);
    Compare({}, {}, true);
    auto owner = std::make_shared<std::array<int, 2>>();
    Compare(std::shared_ptr<int>(owner, &(*owner)[0]), std::shared_ptr<int>(owner, &(*owner)[1]), false);
    Compare(std::shared_ptr<int>(std::shared_ptr<int>(), &object),
            std::shared_ptr<int>(std::shared_ptr<int>(), &object), true);
}

void Lifetime() {
    AtomicSharedPtr<int> value(std::make_shared<int>(1));
    bool deleted = false;
    bool observed = false;
    auto expected = std::shared_ptr<int>(new int(2), [&](int* pointer) {
        deleted = true;
        observed = *value.load(std::memory_order_acquire) == 1;
        delete pointer;
    });
    Require(!value.compare_exchange_strong(expected, std::make_shared<int>(3), std::memory_order_acq_rel),
            "compare_exchange_strong replaced a different value");
    Require(deleted && observed && *expected == 1, "failed comparison did not release the old expected pointer");

    deleted = false;
    observed = false;
    value.store(std::shared_ptr<int>(new int(4), [&](int* pointer) {
        deleted = true;
        observed = *value.load(std::memory_order_acquire) == 5;
        delete pointer;
    }), std::memory_order_release);
    expected = value.load(std::memory_order_acquire);
    Require(value.compare_exchange_strong(expected, std::make_shared<int>(5), std::memory_order_acq_rel),
            "compare_exchange_strong rejected the current owner");
    Require(!deleted && *expected == 4, "successful comparison released the expected pointer too early");
    expected.reset();
    Require(deleted && observed, "successful comparison lost the old pointer's lifetime");

    deleted = false;
    observed = false;
    value.store(std::shared_ptr<int>(new int(6), [&](int* pointer) {
        deleted = true;
        observed = *value.load(std::memory_order_acquire) == 7;
        delete pointer;
    }), std::memory_order_release);
    value.store(std::make_shared<int>(7), std::memory_order_release);
    Require(deleted && observed, "store did not release its old value after publishing the replacement");

    deleted = false;
    observed = false;
    value.store(std::shared_ptr<int>(new int(8), [&](int* pointer) {
        deleted = true;
        observed = *value.load(std::memory_order_acquire) == 9;
        delete pointer;
    }), std::memory_order_release);
    auto previous = value.exchange(std::make_shared<int>(9), std::memory_order_acq_rel);
    Require(!deleted && *previous == 8, "exchange released its returned value too early");
    previous.reset();
    Require(deleted && observed, "exchange lost the returned pointer's lifetime");
}

void Concurrent() {
    constexpr unsigned Threads = 4;
    constexpr unsigned Iterations = 250;
    AtomicSharedPtr<const unsigned> value(std::make_shared<const unsigned>(0));
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < Threads; ++thread) {
        workers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned iteration = 0; iteration < Iterations; ++iteration) {
                auto expected = value.load(std::memory_order_acquire);
                for (;;) {
                    auto desired = std::make_shared<const unsigned>(*expected + 1);
                    if (value.compare_exchange_strong(expected, std::move(desired), std::memory_order_acq_rel)) break;
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    Require(*value.load(std::memory_order_acquire) == Threads * Iterations, "concurrent comparisons lost an update");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected ownership, lifetime or concurrent");
        if (std::strcmp(argv[1], "ownership") == 0) Ownership();
        else if (std::strcmp(argv[1], "lifetime") == 0) Lifetime();
        else if (std::strcmp(argv[1], "concurrent") == 0) Concurrent();
        else throw std::runtime_error("unknown atomic shared pointer test");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
