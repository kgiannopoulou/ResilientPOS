#include "resilientpos/server/HttpTransactionServer.hpp"

#include "resilientpos/server/TransactionHandler.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <memory>
#include <utility>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

namespace resilientpos::server {

namespace {

constexpr auto kSessionTimeout = std::chrono::seconds{10};
constexpr std::size_t kMaxBodyBytes = 16 * 1024;

// One accepted connection: read a request, write the response, close.
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, ProcessedTransactionStore& store,
            const HttpTransactionServer::Logger& logger)
        : stream_{std::move(socket)}
        , store_{store}
        , logger_{logger}
    {
        parser_.body_limit(kMaxBodyBytes);
    }

    void start()
    {
        // A client that connects and never sends anything must not hold the
        // (single-threaded) server forever.
        stream_.expires_after(kSessionTimeout);
        http::async_read(stream_, buffer_, parser_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                self->onRead(ec);
            });
    }

private:
    void onRead(beast::error_code ec)
    {
        if (ec) {
            return;  // client went away or timed out; nothing to answer
        }

        const auto& request = parser_.get();
        response_.version(request.version());
        response_.keep_alive(false);
        response_.set(http::field::server, "ResilientPOS-Server");
        response_.set(http::field::content_type, "application/json");

        if (request.target() != "/transactions") {
            response_.result(http::status::not_found);
            response_.body() = R"({"status":"REJECTED","error":"unknown path"})";
        } else if (request.method() != http::verb::post) {
            response_.result(http::status::method_not_allowed);
            response_.set(http::field::allow, "POST");
            response_.body() = R"({"status":"REJECTED","error":"use POST"})";
        } else {
            const auto result = handleTransactionRequest(request.body(), store_);
            response_.result(result.httpStatus);
            response_.body() = result.body;
            if (logger_) {
                logger_(std::string{request.body()} + " -> " + result.body);
            }
        }
        response_.prepare_payload();

        http::async_write(stream_, response_,
            [self = shared_from_this()](beast::error_code, std::size_t) {
                beast::error_code ignored;
                self->stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
            });
    }

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request_parser<http::string_body> parser_;
    http::response<http::string_body> response_;
    ProcessedTransactionStore& store_;
    const HttpTransactionServer::Logger& logger_;
};

} // namespace

struct HttpTransactionServer::Impl {
    Impl(ProcessedTransactionStore& s, const std::string& address, std::uint16_t port, Logger l)
        : store{s}
        , logger{std::move(l)}
        , acceptor{ioc, tcp::endpoint{asio::ip::make_address(address), port}}
    {
    }

    void accept()
    {
        acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (ec) {
                return;  // acceptor closed by stop()
            }
            std::make_shared<Session>(std::move(socket), store, logger)->start();
            accept();
        });
    }

    ProcessedTransactionStore& store;
    Logger logger;
    asio::io_context ioc;
    tcp::acceptor acceptor;
};

HttpTransactionServer::HttpTransactionServer(
    ProcessedTransactionStore& store,
    const std::string& address,
    std::uint16_t port,
    Logger logger)
    : impl_{std::make_unique<Impl>(store, address, port, std::move(logger))}
{
}

HttpTransactionServer::~HttpTransactionServer() = default;

std::uint16_t HttpTransactionServer::port() const noexcept
{
    return impl_->acceptor.local_endpoint().port();
}

void HttpTransactionServer::run(bool handleSignals)
{
    asio::signal_set signals{impl_->ioc};
    if (handleSignals) {
        signals.add(SIGINT);
        signals.add(SIGTERM);
        signals.async_wait([this](beast::error_code ec, int) {
            if (!ec) {
                stop();
            }
        });
    }

    impl_->accept();
    impl_->ioc.run();
}

void HttpTransactionServer::stop()
{
    asio::post(impl_->ioc, [this] {
        beast::error_code ignored;
        impl_->acceptor.close(ignored);
        impl_->ioc.stop();
    });
}

} // namespace resilientpos::server
