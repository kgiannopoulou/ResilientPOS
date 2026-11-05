#include "resilientpos/observability/OpsEndpoints.hpp"

#include <boost/json.hpp>

#include <exception>

namespace json = boost::json;

namespace resilientpos::observability {

namespace {

constexpr std::string_view kJson = "application/json";
constexpr std::string_view kPrometheus = "text/plain; version=0.0.4; charset=utf-8";

OpsResponse jsonResponse(unsigned int httpStatus, const json::object& body)
{
    return {httpStatus, std::string{kJson}, json::serialize(body)};
}

OpsResponse readinessResponse(const OpsEndpoints& endpoints)
{
    Readiness readiness;
    try {
        readiness = endpoints.readiness();
    } catch (const std::exception& e) {
        // A check that blows up is an unready process, not an unanswered probe.
        readiness = Readiness{false, {{"error", e.what()}}};
    }

    json::object checks;
    for (const auto& [name, result] : readiness.checks) {
        checks[name] = result;
    }
    return jsonResponse(readiness.ready ? 200 : 503, json::object{
        {"status", readiness.ready ? "READY" : "NOT_READY"},
        {"checks", checks},
    });
}

} // namespace

std::optional<OpsResponse> handleOpsRequest(
    std::string_view method,
    std::string_view target,
    const OpsEndpoints& endpoints)
{
    const std::string_view path = target.substr(0, target.find('?'));
    if (path != "/health" && path != "/ready" && path != "/metrics") {
        return std::nullopt;
    }
    if (method != "GET") {
        return jsonResponse(405, json::object{{"status", "REJECTED"}, {"error", "use GET"}});
    }

    if (path == "/health") {
        return jsonResponse(200, json::object{{"status", "UP"}});
    }
    if (path == "/ready") {
        return readinessResponse(endpoints);
    }
    return OpsResponse{200, std::string{kPrometheus}, endpoints.metrics()};
}

} // namespace resilientpos::observability
