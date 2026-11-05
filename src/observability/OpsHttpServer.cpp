#include "resilientpos/observability/OpsHttpServer.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

namespace resilientpos::observability {

namespace {

constexpr auto kSessionTimeout = std::chrono::seconds{5};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, const OpsEndpoints& endpoints)
        : stream_{std::move(socket)}
        , endpoints_{endpoints}
    {
    }

    void start()
    {
        stream_.expires_after(kSessionTimeout);
        http::async_read(stream_, buffer_, request_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (!ec) {
                    self->answer();
                }
            });
    }

private:
    void answer()
    {
        const auto target = request_.target();
        const auto method = request_.method_string();
        auto ops = handleOpsRequest(std::string_view{method.data(), method.size()},
                                    std::string_view{target.data(), target.size()}, endpoints_);
        if (!ops) {
            ops = OpsResponse{404, "application/json", R"({"status":"REJECTED","error":"unknown path"})"};
        }

        response_.version(request_.version());
        response_.keep_alive(false);
        response_.result(static_cast<http::status>(ops->httpStatus));
        response_.set(http::field::server, "ResilientPOS-Till");
        response_.set(http::field::content_type, ops->contentType);
        response_.body() = std::move(ops->body);
        response_.prepare_payload();

        http::async_write(stream_, response_,
            [self = shared_from_this()](beast::error_code, std::size_t) {
                beast::error_code ignored;
                (void)self->stream_.socket().shutdown(tcp::socket::shutdown_send, ignored);
            });
    }

    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> request_;
    http::response<http::string_body> response_;
    const OpsEndpoints& endpoints_;
};

} // namespace

struct OpsHttpServer::Impl {
    Impl(OpsEndpoints ops, const std::string& address, std::uint16_t port)
        : endpoints{std::move(ops)}
        , acceptor{ioc, tcp::endpoint{asio::ip::make_address(address), port}}
        , boundPort{acceptor.local_endpoint().port()}
    {
    }

    void accept()
    {
        acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (ec == asio::error::operation_aborted || !acceptor.is_open()) {
                return;
            }
            if (!ec) {
                std::make_shared<Session>(std::move(socket), endpoints)->start();
            }
            accept();
        });
    }

    OpsEndpoints endpoints;
    asio::io_context ioc;
    tcp::acceptor acceptor;
    std::uint16_t boundPort;
    std::jthread thread;  // last: joined before anything it uses goes away
};

OpsHttpServer::OpsHttpServer(OpsEndpoints endpoints, const std::string& address, std::uint16_t port)
    : impl_{std::make_unique<Impl>(std::move(endpoints), address, port)}
{
    impl_->accept();
    impl_->thread = std::jthread{[impl = impl_.get()] {
        try {
            impl->ioc.run();
        } catch (const std::exception& e) {
            spdlog::error("Metrics listener stopped: {}", e.what());
        }
    }};
}

OpsHttpServer::~OpsHttpServer()
{
    impl_->ioc.stop();
    impl_->thread.join();
}

std::uint16_t OpsHttpServer::port() const noexcept
{
    return impl_->boundPort;
}

} // namespace resilientpos::observability
