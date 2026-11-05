#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace resilientpos::observability {

// Is this process able to do its job right now, and what did it check?
struct Readiness {
    bool ready{false};
    std::map<std::string, std::string> checks;
};

// What a process plugs in to expose its operational endpoints.
struct OpsEndpoints {
    std::function<std::string()> metrics;    // Prometheus text
    std::function<Readiness()> readiness;
};

struct OpsResponse {
    unsigned int httpStatus;
    std::string contentType;
    std::string body;
};

// The operational endpoints, independent of any socket code, shared by the
// central server and the till's optional listener:
//
//   GET /health   200 {"status":"UP"}            the process is alive
//   GET /ready    200 {"status":"READY",...}     it can do its job
//                 503 {"status":"NOT_READY",...}
//   GET /metrics  200 Prometheus text format
//
// /health and /ready answer different questions: a failing /health means
// restart the process, a failing /ready means send it no traffic yet.
//
// Returns nullopt if `target` is not one of these paths. Any method but GET on
// one of them is a 405.
[[nodiscard]]
std::optional<OpsResponse> handleOpsRequest(
    std::string_view method,
    std::string_view target,
    const OpsEndpoints& endpoints
);

} // namespace resilientpos::observability
