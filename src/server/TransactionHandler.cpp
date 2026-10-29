#include "resilientpos/server/TransactionHandler.hpp"

#include <boost/json.hpp>

#include <cstdint>
#include <exception>

namespace json = boost::json;

namespace resilientpos::server {

namespace {

HandlerResponse reply(unsigned int httpStatus, std::string_view status, std::string_view error = {})
{
    json::object body{{"status", status}};
    if (!error.empty()) {
        body["error"] = error;
    }
    return {httpStatus, json::serialize(body)};
}

} // namespace

HandlerResponse handleTransactionRequest(std::string_view body, ProcessedTransactionStore& store)
{
    std::error_code parseError;
    const json::value request = json::parse(body, parseError);
    if (parseError) {
        return reply(400, "REJECTED", "body is not valid JSON");
    }

    const auto* object = request.if_object();
    if (object == nullptr) {
        return reply(400, "REJECTED", "body must be a JSON object");
    }

    const auto* id = object->if_contains("transaction_id");
    if (id == nullptr || !id->is_string() || id->get_string().empty()) {
        return reply(400, "REJECTED", "transaction_id must be a non-empty string");
    }

    // Money travels as integer cents, never as a float.
    const auto* amount = object->if_contains("amount_cents");
    if (amount == nullptr || !amount->is_int64()) {
        return reply(400, "REJECTED", "amount_cents must be an integer");
    }

    try {
        const auto outcome = store.record(std::string{id->get_string()}, amount->get_int64());
        return reply(200, outcome == RecordOutcome::Inserted ? "ACCEPTED" : "ALREADY_PROCESSED");
    } catch (const std::exception& e) {
        return reply(500, "ERROR", e.what());
    }
}

} // namespace resilientpos::server
