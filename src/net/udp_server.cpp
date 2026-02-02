#include "chwell/net/udp_server.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace net {

UdpServer::UdpServer(asio::io_service& io_service, unsigned short port)
    : io_service_(io_service),
      socket_(io_service, asio::ip::udp::endpoint(asio::ip::udp::v4(), port)),
      buffer_(65536) { // 最大 UDP 报文
}

void UdpServer::start_receive() {
    do_receive();
}

void UdpServer::send_to(const std::vector<char>& data,
                        const asio::ip::udp::endpoint& remote) {
    socket_.async_send_to(asio::buffer(data), remote,
        [](const asio::error_code& ec, std::size_t /*bytes_transferred*/) {
            if (ec) {
                core::Logger::instance().warn("UDP send failed: " + ec.message());
            }
        });
}

void UdpServer::do_receive() {
    socket_.async_receive_from(
        asio::buffer(buffer_), remote_endpoint_,
        [this](const asio::error_code& ec, std::size_t bytes_transferred) {
            on_receive(ec, bytes_transferred);
        });
}

void UdpServer::on_receive(const asio::error_code& ec, std::size_t bytes_transferred) {
    if (ec) {
        if (ec != asio::error::operation_aborted) {
            core::Logger::instance().warn("UDP receive failed: " + ec.message());
        }
        return;
    }

    std::vector<char> data(buffer_.begin(),
                           buffer_.begin() + static_cast<std::ptrdiff_t>(bytes_transferred));

    if (message_cb_) {
        message_cb_(data, remote_endpoint_);
    }

    do_receive();
}

} // namespace net
} // namespace chwell

