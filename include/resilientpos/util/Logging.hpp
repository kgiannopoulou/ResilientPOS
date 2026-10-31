#pragma once

#include <memory>

namespace spdlog {
class logger;
}

namespace resilientpos::util {

// Gives `logger` the project's log line format and makes it the default
// logger the engine writes to:
//
//   [2026-10-27 09:14:02] [INFO] Transaction created id=91ba73f2 amount=550
//   [2026-10-27 09:14:02] [WARN] Server unavailable id=91ba73f2 ...
//
// Messages are key=value pairs after a short sentence, so they read well and
// still grep/parse easily.
void installDefaultLogger(std::shared_ptr<spdlog::logger> logger);

} // namespace resilientpos::util
