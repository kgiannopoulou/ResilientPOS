#include "resilientpos/util/Logging.hpp"

#include <spdlog/pattern_formatter.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string_view>
#include <utility>

namespace resilientpos::util {

namespace {

// %* -> INFO / WARN / ERROR ... (spdlog's own %l prints "info", "warning").
class UpperLevelFlag final : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg& msg, const std::tm&, spdlog::memory_buf_t& dest) override
    {
        const std::string_view name = levelName(msg.level);
        dest.append(name.data(), name.data() + name.size());
    }

    [[nodiscard]]
    std::unique_ptr<custom_flag_formatter> clone() const override
    {
        return std::make_unique<UpperLevelFlag>();
    }

private:
    static std::string_view levelName(spdlog::level::level_enum level) noexcept
    {
        switch (level) {
        case spdlog::level::trace:    return "TRACE";
        case spdlog::level::debug:    return "DEBUG";
        case spdlog::level::info:     return "INFO";
        case spdlog::level::warn:     return "WARN";
        case spdlog::level::err:      return "ERROR";
        case spdlog::level::critical: return "CRITICAL";
        default:                      return "OFF";
        }
    }
};

} // namespace

void installDefaultLogger(std::shared_ptr<spdlog::logger> logger)
{
    auto formatter = std::make_unique<spdlog::pattern_formatter>();
    formatter->add_flag<UpperLevelFlag>('*').set_pattern("[%Y-%m-%d %H:%M:%S] [%^%*%$] %v");
    logger->set_formatter(std::move(formatter));
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
}

} // namespace resilientpos::util
