#include <iostream>
#include <string>
#include <vector>

#include "chwell/core/logger.h"
#include "chwell/net/posix_io.h"
#include "chwell/codec/codec.h"

using namespace chwell;

// JSON 帧客户端：使用 JsonCodec 发送/接收 JSON 字符串（4 字节长度前缀）

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    unsigned short port = 9000;
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = static_cast<unsigned short>(std::atoi(argv[2]));
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket failed\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "invalid host\n";
        return 1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "connect failed\n";
        return 1;
    }

    codec::JsonCodec codec;

    std::cout << "Connected to " << host << ":" << port
              << " (JsonFrame). Enter JSON text and press ENTER to send.\n";

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit" || line == "exit") break;

        std::vector<char> frame = codec.encode(line);
        const char* p = frame.data();
        std::size_t len = frame.size();
        while (len > 0) {
            ssize_t n = ::write(fd, p, len);
            if (n <= 0) {
                std::cerr << "write failed\n";
                return 1;
            }
            p += n;
            len -= static_cast<std::size_t>(n);
        }

        char buf[4096];
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
            std::cerr << "server closed\n";
            break;
        }

        std::vector<char> recv_buf(buf, buf + n);
        std::vector<std::string> msgs = codec.decode(recv_buf);
        for (const auto& m : msgs) {
            std::cout << "RECV: " << m << "\n";
        }
    }

    ::close(fd);
    return 0;
}
