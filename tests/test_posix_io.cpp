#include "chwell/net/posix_io.h" // Legacy umbrella retains native POSIX access.
#include <gtest/gtest.h>
#include <future>
#include <type_traits>
#include <utility>

namespace {
struct Pair {
    int fd[2]{-1, -1};
    Pair() { if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fd) != 0) throw std::runtime_error("socketpair"); }
    ~Pair() { for (int value : fd) if (value >= 0) ::close(value); }
    int take(int index) { return std::exchange(fd[index], -1); }
};
}
TEST(PosixSocketBoundary, BinaryReadWriteAndMoveKeepSingleOwner) {
    Pair pair;
    chwell::net::TcpSocket sender(pair.take(0)), receiver(pair.take(1));
    const int original = sender.native_handle();
    chwell::net::TcpSocket moved(std::move(sender));
    EXPECT_FALSE(sender.is_open());
    EXPECT_EQ(moved.native_handle(), original);
    const std::string bytes("a\0b", 3);
    ASSERT_EQ(moved.write(bytes.data(), bytes.size()), 3);
    char buffer[3];
    ASSERT_EQ(receiver.read(buffer, sizeof(buffer)), 3);
    EXPECT_EQ(std::string(buffer, sizeof(buffer)), bytes);
    chwell::net::ErrorCode error;
    moved.close(error);
    EXPECT_FALSE(error);
    EXPECT_EQ(::fcntl(original, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    moved.close(error);
    EXPECT_FALSE(error);
}
TEST(PosixSocketBoundary, MoveAssignmentClosesPreviousDescriptor) {
    Pair pair;
    chwell::net::TcpSocket old(pair.take(0)), replacement(pair.take(1));
    const int discarded = old.native_handle(), retained = replacement.native_handle();
    old = std::move(replacement);
    EXPECT_FALSE(replacement.is_open());
    EXPECT_EQ(old.native_handle(), retained);
    EXPECT_EQ(::fcntl(discarded, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}
TEST(PosixSocketBoundary, ClosedReadWriteReportBadDescriptor) {
    chwell::net::TcpSocket socket;
    char buffer{};
    errno = 0;
    EXPECT_EQ(socket.read(&buffer, 1), -1);
    EXPECT_EQ(errno, EBADF);
    errno = 0;
    EXPECT_EQ(socket.write(&buffer, 1), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(chwell::net::ErrorCode{}.message(), "Success");
    EXPECT_FALSE(chwell::net::ErrorCode(EBADF).message().empty());
}
TEST(PosixSocketBoundary, ShutdownWakesReadWithoutClosingDescriptor) {
    Pair pair;
    chwell::net::TcpSocket reader(pair.take(0));
    const int fd = reader.native_handle();
    auto blocked = std::async(std::launch::async, [&] { char buffer; return reader.read(&buffer, 1); });
    chwell::net::ErrorCode error;
    reader.shutdown(SHUT_RDWR, error);
    EXPECT_FALSE(error);
    EXPECT_EQ(blocked.get(), 0);
    EXPECT_TRUE(reader.is_open());
    EXPECT_GE(::fcntl(fd, F_GETFD), 0);
}
TEST(PosixSocketBoundary, AcceptorConnectsOverRealLoopback) {
    chwell::net::TcpAcceptor acceptor(0);
    ASSERT_GE(acceptor.listen_fd(), 0);
    sockaddr_in bound{};
    socklen_t size = sizeof(bound);
    ASSERT_EQ(::getsockname(acceptor.listen_fd(), reinterpret_cast<sockaddr*>(&bound), &size), 0);
    bound.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    chwell::net::TcpSocket client(::socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_TRUE(client.is_open());
    ASSERT_EQ(::connect(client.native_handle(), reinterpret_cast<sockaddr*>(&bound), sizeof(bound)), 0);
    chwell::net::ErrorCode error;
    auto accepted = acceptor.accept(error);
    ASSERT_FALSE(error);
    ASSERT_TRUE(accepted.is_open());
    ASSERT_EQ(client.write("ping", 4), 4);
    char buffer[4];
    ASSERT_EQ(accepted.read(buffer, 4), 4);
    EXPECT_EQ(std::string(buffer, 4), "ping");
    chwell::net::UdpEndpoint endpoint("127.0.0.1", ntohs(bound.sin_port));
    EXPECT_EQ(endpoint.address(), "127.0.0.1");
    EXPECT_EQ(endpoint.port(), ntohs(bound.sin_port));
}
