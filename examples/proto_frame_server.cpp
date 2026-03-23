#include <csignal>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>

#include "chwell/core/logger.h"
#include "chwell/core/config.h"
#include "chwell/service/service.h"
#include "chwell/codec/codec.h"

using namespace chwell;

// 纯 Protobuf 帧服务示例：
// - 顶层协议不再使用 protocol::Message/cmd
// - 一条 TCP 连接上直接收发 ProtobufCodec 定义的帧：
//   [len(varint32)][protobuf bytes]...
// 为了简化演示，这里直接把 payload 当作 UTF-8 字符串回显；
// 实际业务中可用 game.pb.h 的 C2S_Login 等 ParseFromArray/SerializeAsString 做真实 Protobuf 编解码。

class ProtoFrameComponent : public service::Component {
public:
    virtual std::string name() const override {
        return "ProtoFrameComponent";
    }

    virtual void on_message(const net::TcpConnectionPtr& conn,
                            std::string_view data) override {
        auto& codec = codecs_[conn.get()];
        std::vector<char> chunk(data.begin(), data.end());
        std::vector<std::string> messages = codec.decode(chunk);

        for (const auto& bin : messages) {
            std::string text(bin.begin(), bin.end());
            CHWELL_LOG_INFO("ProtoFrame received size=" +
                            std::to_string(bin.size()) + " body=" + text);

            // demo：在真实业务中，这里应该 ParseFromArray / SerializeToString
            std::string reply_payload = "server echo: " + text;

            codec::ProtobufCodec tmp_codec;
            std::vector<char> frame = tmp_codec.encode(reply_payload);
            conn->send(frame);
        }
    }

    virtual void on_disconnect(const net::TcpConnectionPtr& conn) override {
        codecs_.erase(conn.get());
    }

private:
    std::unordered_map<const net::TcpConnection*, codec::ProtobufCodec> codecs_;
};

int main() {
    CHWELL_LOG_INFO("Starting ProtoFrame Server (pure protobuf framing)...");

    core::Config cfg;
    cfg.load_from_file("server.conf");

    service::Service svc(static_cast<unsigned short>(cfg.listen_port()),
                         static_cast<std::size_t>(cfg.worker_threads()));

    svc.add_component<ProtoFrameComponent>();

    svc.start();

    CHWELL_LOG_INFO("ProtoFrame Server running on port " +
                    std::to_string(cfg.listen_port()));

    static volatile sig_atomic_t g_stop = 0;
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });

    if (isatty(STDIN_FILENO)) {
        std::cout << "Press ENTER to exit..." << std::endl;
        std::string line;
        std::getline(std::cin, line);
    } else {
        while (!g_stop) {
            sleep(1);
        }
    }

    svc.stop();
    return 0;
}

