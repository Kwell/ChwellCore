#include <gtest/gtest.h>
#include "chwell/net/epoll_server.h"
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <future>

namespace {
struct Socket {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ~Socket() { if (fd >= 0) ::close(fd); }
};
unsigned short free_port() {
    Socket socket;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (socket.fd < 0 || ::bind(socket.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) return 0;
    socklen_t size = sizeof(address);
    if (::getsockname(socket.fd, reinterpret_cast<sockaddr*>(&address), &size)) return 0;
    return ntohs(address.sin_port);
}
bool connect_to(int fd, unsigned short port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    return ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
}
}

TEST(EpollAcceptOrder, InitialClientDataWaitsForConnectionInitialization) {
    using namespace std::chrono_literals;
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto release_future = release.get_future();
    std::promise<bool> message;
    auto message_future = message.get_future();
    std::atomic<bool> initialized{false};
    const auto port = free_port(); ASSERT_GT(port, 0);
    chwell::net::EpollTcpServer server(port);
    server.set_connection_callback([&](const auto&) {
        entered.set_value();
        release_future.wait_for(2s);
        initialized = true;
    });
    server.set_message_callback([&](const auto&, std::string_view) { message.set_value(initialized.load()); });
    ASSERT_TRUE(server.start_checked());
    Socket client; ASSERT_TRUE(connect_to(client.fd, port));
    ASSERT_EQ(entered_future.wait_for(1s), std::future_status::ready);
    ASSERT_EQ(::send(client.fd, "x", 1, MSG_NOSIGNAL), 1);
    const auto early = message_future.wait_for(200ms);
    // Always release before assertions/teardown, including on regression.
    release.set_value();
    EXPECT_EQ(early, std::future_status::timeout);
    ASSERT_EQ(message_future.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(message_future.get());
    server.stop();
}

TEST(EpollAcceptOrder, GreetingQueuedDuringInitializationIsSentAfterRegistration) {
    const auto port = free_port(); ASSERT_GT(port, 0);
    chwell::net::EpollTcpServer server(port);
    server.set_connection_callback([](const auto& connection) { connection->send(std::string_view("hello")); });
    ASSERT_TRUE(server.start_checked());
    Socket client; ASSERT_TRUE(connect_to(client.fd, port));
    std::string received;
    while (received.size() < 5) {
        pollfd pending{client.fd, POLLIN, 0};
        ASSERT_GT(::poll(&pending, 1, 1000), 0);
        char buffer[5];
        const auto size = ::recv(client.fd, buffer, 5 - received.size(), 0);
        ASSERT_GT(size, 0);
        received.append(buffer, static_cast<std::size_t>(size));
    }
    EXPECT_EQ(received, "hello");
    server.stop();
}
