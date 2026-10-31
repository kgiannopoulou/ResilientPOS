#pragma once

#include <chrono>
#include <thread>

namespace resilientpos::test {

// Waits until `condition()` holds, checking every few milliseconds, and gives
// up after `timeout`. For asserting on work a background thread does.
template <typename Condition>
bool eventually(Condition condition, std::chrono::milliseconds timeout = std::chrono::seconds{10})
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return condition();
}

} // namespace resilientpos::test
