#include "runtime/interruptible_slot.h"
#include <future>
#include <iostream>
#include <memory>
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " #x << '\n'; return 1; } } while (0)
int main()
{
    runtime::InterruptibleSlot<std::shared_ptr<int>> slot;
    std::atomic<bool> stop{false};
    auto sample = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = sample;
    slot.publish(sample);
    sample.reset();
    std::shared_ptr<int> output;
    CHECK(slot.wait(output, stop) && *output == 42);
    CHECK(!lifetime.expired());
    output.reset();
    CHECK(lifetime.expired());
    auto pending = std::async(std::launch::async, [&] { return slot.wait(output, stop); });
    stop.store(true); // driver never supplies the outstanding callback
    CHECK(pending.wait_for(1s) == std::future_status::ready);
    CHECK(!pending.get());
    stop.store(false);
    auto closed = std::async(std::launch::async, [&] { return slot.wait(output, stop); });
    slot.close();
    CHECK(closed.wait_for(1s) == std::future_status::ready && !closed.get());
    auto late = std::make_shared<int>(99);
    std::weak_ptr<int> lateLifetime = late;
    slot.publish(std::move(late)); // cancelled reader may call back after teardown
    CHECK(lateLifetime.expired());
    std::cout << "interruptible callback slot: passed\n";
}
