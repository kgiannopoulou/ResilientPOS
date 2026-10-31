// Keeps the engine's diagnostic logging out of test output. Set
// RESILIENTPOS_TEST_LOGS=1 to see it while debugging a failing test.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // std::getenv is fine for a read-only flag
#endif

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>

namespace {

class QuietLogging final : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override
    {
        if (std::getenv("RESILIENTPOS_TEST_LOGS") == nullptr) {
            spdlog::set_level(spdlog::level::off);
        }
    }
};

} // namespace

CATCH_REGISTER_LISTENER(QuietLogging)
