#include "resilientpos/network/HttpServerClient.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include <exception>
#include <string_view>
#include <utility>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
using tcp = asio::ip::tcp;

namespace resilientpos::network {

namespace {

SendResult networkError(std::string_view stage, const beast::error_code& ec)
{
    return {SendResultType::NetworkError, std::string{stage} + ": " + ec.message()};
}

SendResult serverError(std::string message)
{
    return {SendResultType::ServerError, std::move(message)};
}

// Runs one async operation to completion (or until the stream's deadline
// fires) and returns its error code. Using async calls on a private
// io_context is what gives us timeouts; the blocking API has none.
template <typename Start>
beast::error_code runOperation(asio::io_context& ioc, Start&& start)
{
    beast::error_code result = asio::error::would_block;
    start([&result](beast::error_code ec, auto&&...) { result = ec; });
    ioc.restart();
    ioc.run();
    return result;
}

SendResult interpretResponse(const http::response<http::string_body>& response)
{
    const auto statusCode = response.result_int();
    if (statusCode < 200 || statusCode >= 300) {
        // `response` is a const reference; the analyzer follows Beast's
        // internal moves into it and misreports it as moved-from.
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.Move)
        return serverError("HTTP " + std::to_string(statusCode) + ": " + response.body());
    }

    std::error_code parseError;
    const json::value body = json::parse(response.body(), parseError);
    const auto* object = body.if_object();
    const auto* status = object != nullptr ? object->if_contains("status") : nullptr;
    if (parseError || status == nullptr || !status->is_string()) {
        return serverError("Unexpected reply: " + response.body());
    }

    const std::string_view text = status->get_string();
    if (text == "ACCEPTED") {
        return {SendResultType::Accepted, "Transaction accepted"};
    }
    if (text == "ALREADY_PROCESSED") {
        return {SendResultType::AlreadyProcessed, "Transaction already processed"};
    }
    return serverError("Server answered " + std::string{text});
}

} // namespace

HttpServerClient::HttpServerClient(std::string host, std::uint16_t port,
                                   std::chrono::milliseconds timeout)
    : host_{std::move(host)}
    , port_{port}
    , timeout_{timeout}
{
}

SendResult HttpServerClient::send(const core::Transaction& transaction)
{
    try {
        asio::io_context ioc;
        tcp::resolver resolver{ioc};
        beast::tcp_stream stream{ioc};

        tcp::resolver::results_type endpoints;
        auto ec = runOperation(ioc, [&](auto handler) {
            resolver.async_resolve(host_, std::to_string(port_),
                [&endpoints, handler](beast::error_code e, tcp::resolver::results_type r) {
                    endpoints = std::move(r);
                    handler(e);
                });
        });
        if (ec) {
            return networkError("Cannot resolve " + host_, ec);
        }

        stream.expires_after(timeout_);
        ec = runOperation(ioc, [&](auto handler) { stream.async_connect(endpoints, handler); });
        if (ec) {
            return networkError("Cannot connect to " + host_ + ":" + std::to_string(port_), ec);
        }

        http::request<http::string_body> request{http::verb::post, "/transactions", 11};
        request.set(http::field::host, host_);
        request.set(http::field::content_type, "application/json");
        request.set(http::field::user_agent, "ResilientPOS");
        request.body() = json::serialize(json::object{
            {"transaction_id", transaction.id},
            {"amount_cents", transaction.amount.cents()},
        });
        request.prepare_payload();

        stream.expires_after(timeout_);
        ec = runOperation(ioc, [&](auto handler) { http::async_write(stream, request, handler); });
        if (ec) {
            return networkError("Sending request failed", ec);
        }

        // From here on the server may already have stored the transaction even
        // if we never see its reply. That is fine: the sale stays PENDING_SYNC,
        // is resent later and the server answers ALREADY_PROCESSED.
        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        stream.expires_after(timeout_);
        ec = runOperation(ioc, [&](auto handler) {
            http::async_read(stream, buffer, response, handler);
        });
        if (ec) {
            return networkError("No reply from server", ec);
        }

        beast::error_code ignored;
        (void)stream.socket().shutdown(tcp::socket::shutdown_both, ignored);

        return interpretResponse(response);
    } catch (const std::exception& e) {
        // Contract: expected failures are reported, not thrown.
        return {SendResultType::NetworkError, e.what()};
    }
}

} // namespace resilientpos::network
