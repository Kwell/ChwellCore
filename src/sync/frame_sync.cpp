#include "chwell/sync/frame_sync.h"
#include "chwell/service/protocol_router.h"
#include "chwell/protocol/message.h"
#include "chwell/core/endian.h"
#include "chwell/core/timer_wheel.h"
#include <cstring>

namespace chwell {
namespace sync {

// ============================================
// 辅助函数：编码/解码（不变）
// ============================================

static bool decode_uint32(const char* data, size_t size, size_t& offset, uint32_t& out) {
    if (offset + 4 > size) return false;
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        value |= (static_cast<uint32_t>(static_cast<uint8_t>(data[offset + i])) << (i * 8));
    }
    out = value;
    offset += 4;
    return true;
}

static std::string encode_uint32(uint32_t value) {
    std::string result;
    result.resize(4);
    for (int i = 0; i < 4; i++) {
        result[i] = static_cast<char>((value >> (i * 8)) & 0xFF);
    }
    return result;
}

static bool decode_uint8(const char* data, size_t size, size_t& offset, uint8_t& out) {
    if (offset + 1 > size) return false;
    out = static_cast<uint8_t>(data[offset]);
    offset += 1;
    return true;
}

static std::string encode_uint8(uint8_t value) {
    return std::string(1, static_cast<char>(value));
}

static bool decode_bytes(const char* data, size_t size, size_t& offset, std::vector<uint8_t>& out) {
    if (offset + 2 > size) return false;
    uint16_t len_net;
    std::memcpy(&len_net, data + offset, 2);
    uint16_t len = core::net_to_host16(len_net);
    offset += 2;
    if (offset + len > size) return false;
    out.assign(data + offset, data + offset + len);
    offset += len;
    return true;
}

static std::string encode_bytes(const std::vector<uint8_t>& data) {
    std::string result;
    uint16_t len = core::host_to_net16(static_cast<uint16_t>(data.size()));
    result.append(reinterpret_cast<const char*>(&len), 2);
    result.insert(result.end(), data.begin(), data.end());
    return result;
}

// ============================================
// FrameSyncComponent
// ============================================

void FrameSyncComponent::on_register(service::Service& svc) {
    auto* router = svc.get_component<service::ProtocolRouterComponent>();
    if (router) {
        router->register_handler(frame_cmd::C2S_FRAME_INPUT,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                this->handle_frame_input(conn, msg.body);
            });

        router->register_handler(frame_cmd::C2S_FRAME_SYNC_REQ,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                this->handle_frame_sync_req(conn, msg.body);
            });

        CHWELL_LOG_INFO("FrameSyncComponent registered handlers");
    } else {
        CHWELL_LOG_WARN("ProtocolRouterComponent not found");
    }
}

bool FrameSyncComponent::PreUpdate() {
    // 🆕 启动帧超时检测定时器
    // 每帧间隔调用一次，检查所有房间是否有超时帧
    int tick_ms = 1000 / frame_rate_;
    frame_timer_handle_ = core::TimerManager::instance().add_repeat_timer(tick_ms,
        [this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& pair : rooms_) {
                if (pair.second->check_frame_timeout()) {
                    // 超时推进，广播帧状态
                    FrameState state;
                    state.frame_id = pair.second->current_frame();
                    state.state_data.clear();
                    broadcast_frame_state(pair.first, state);
                }
            }
        });

    if (frame_timer_handle_.valid()) {
        CHWELL_LOG_INFO("FrameSyncComponent: frame timeout timer started (interval="
                         << tick_ms << "ms)");
    }
    return true;
}

bool FrameSyncComponent::Shut() {
    if (frame_timer_handle_.valid()) {
        core::TimerManager::instance().cancel_timer(frame_timer_handle_);
        CHWELL_LOG_INFO("FrameSyncComponent: frame timeout timer cancelled");
    }
    return true;
}

void FrameSyncComponent::handle_frame_input(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    const char* ptr = data.data();
    size_t size = data.size();
    size_t offset = 0;

    uint32_t player_id = 0;
    uint32_t frame_id = 0;
    std::vector<uint8_t> input_data;

    if (!decode_uint32(ptr, size, offset, player_id)) {
        CHWELL_LOG_ERROR("Failed to decode player_id");
        return;
    }

    if (!decode_uint32(ptr, size, offset, frame_id)) {
        CHWELL_LOG_ERROR("Failed to decode frame_id");
        return;
    }

    if (!decode_bytes(ptr, size, offset, input_data)) {
        CHWELL_LOG_ERROR("Failed to decode input_data");
        return;
    }

    CHWELL_LOG_INFO("Frame input: player_id=" + std::to_string(player_id) +
                    ", frame_id=" + std::to_string(frame_id) +
                    ", data_size=" + std::to_string(input_data.size()));

    FrameInput input;
    input.frame_id = frame_id;
    input.player_id = player_id;
    input.input_data = input_data;

    submit_input(player_id, input);

    // 检查是否所有玩家都提交了输入
    std::string room_id = get_room_id(conn);
    if (!room_id.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = rooms_.find(room_id);
        if (it != rooms_.end()) {
            auto& room = it->second;
            if (room->all_inputs_ready(frame_id)) {
                room->advance_frame();

                FrameState state;
                state.frame_id = room->current_frame();
                state.state_data.clear();
                broadcast_frame_state(room_id, state);
            }
        }
    }
}

void FrameSyncComponent::handle_frame_sync_req(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    const char* ptr = data.data();
    size_t size = data.size();
    size_t offset = 0;

    if (size < 2) {
        CHWELL_LOG_ERROR("handle_frame_sync_req: data too short (< 2 bytes)");
        return;
    }

    uint16_t room_id_len_net;
    std::memcpy(&room_id_len_net, ptr + offset, 2);
    uint16_t room_id_len = core::net_to_host16(room_id_len_net);
    offset += 2;

    if (offset + room_id_len > size) {
        CHWELL_LOG_ERROR("Failed to decode room_id");
        return;
    }

    std::string room_id(ptr + offset, room_id_len);

    CHWELL_LOG_INFO("Frame sync request: room_id=" + room_id);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = rooms_.find(room_id);
    if (it != rooms_.end()) {
        auto& room = it->second;
        send_frame_sync(conn, room->current_frame());
    } else {
        CHWELL_LOG_WARN("Room not found: " + room_id);
    }
}

void FrameSyncComponent::create_room(const std::string& room_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (rooms_.find(room_id) != rooms_.end()) {
        CHWELL_LOG_WARN("Room already exists: " + room_id);
        return;
    }

    auto room = std::make_shared<FrameSyncRoom>(room_id, frame_rate_);
    rooms_[room_id] = room;
    room->start_sync();

    CHWELL_LOG_INFO("Created frame sync room: " + room_id);
}

void FrameSyncComponent::destroy_room(const std::string& room_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = rooms_.find(room_id);
    if (it != rooms_.end()) {
        it->second->stop_sync();
        rooms_.erase(it);
        CHWELL_LOG_INFO("Destroyed frame sync room: " + room_id);
    }
}

void FrameSyncComponent::join_room(uint32_t player_id, const std::string& room_id, const net::TcpConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = rooms_.find(room_id);
    if (it == rooms_.end()) {
        CHWELL_LOG_WARN("Room not found: " + room_id);
        return;
    }

    it->second->join_player(player_id, conn);

    ConnectionInfo info;
    info.player_id = player_id;
    info.room_id = room_id;
    connections_[conn.get()] = info;
}

void FrameSyncComponent::leave_room(uint32_t player_id, const std::string& room_id) {
    bool should_destroy = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = rooms_.find(room_id);
        if (it != rooms_.end()) {
            it->second->leave_player(player_id);
            should_destroy = (it->second->player_count() == 0);
        }

        for (auto cit = connections_.begin(); cit != connections_.end(); ) {
            if (cit->second.player_id == player_id && cit->second.room_id == room_id) {
                cit = connections_.erase(cit);
            } else {
                ++cit;
            }
        }
    }

    if (should_destroy) {
        destroy_room(room_id);
    }
}

void FrameSyncComponent::submit_input(uint32_t player_id, const FrameInput& input) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : connections_) {
        if (pair.second.player_id == player_id) {
            std::string room_id = pair.second.room_id;
            auto it = rooms_.find(room_id);
            if (it != rooms_.end()) {
                it->second->submit_input(player_id, input);
                break;
            }
        }
    }
}

void FrameSyncComponent::create_snapshot(const std::string& room_id, const FrameSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = rooms_.find(room_id);
    if (it != rooms_.end()) {
        it->second->create_snapshot(snapshot);
    }
}

void FrameSyncComponent::broadcast_frame_state(const std::string& room_id, const FrameState& state) {
    std::vector<net::TcpConnectionPtr> player_conns;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) {
            return;
        }
        player_conns = it->second->get_player_connections();
    }

    std::string body;
    body += encode_uint32(state.frame_id);
    body += encode_bytes(state.state_data);

    protocol::Message msg(frame_cmd::S2C_FRAME_STATE, std::vector<char>(body.begin(), body.end()));

    for (const auto& conn : player_conns) {
        service::ProtocolRouterComponent::send_message(conn, msg);
    }

    CHWELL_LOG_INFO("Broadcasted frame state to room " + room_id + ", frame_id=" + std::to_string(state.frame_id));
}

void FrameSyncComponent::send_frame_sync(const net::TcpConnectionPtr& conn, uint32_t current_frame) {
    std::string body;
    body += encode_uint32(current_frame);

    protocol::Message msg(frame_cmd::S2C_FRAME_SYNC, std::vector<char>(body.begin(), body.end()));
    service::ProtocolRouterComponent::send_message(conn, msg);

    CHWELL_LOG_DEBUG("Sent frame sync: current_frame=" + std::to_string(current_frame));
}

void FrameSyncComponent::send_frame_snapshot(const net::TcpConnectionPtr& conn, const FrameSnapshot& snapshot) {
    std::string body;
    body += encode_uint32(snapshot.frame_id);
    body += encode_bytes(snapshot.snapshot_data);

    protocol::Message msg(frame_cmd::S2C_FRAME_SNAPSHOT, std::vector<char>(body.begin(), body.end()));
    service::ProtocolRouterComponent::send_message(conn, msg);

    CHWELL_LOG_DEBUG("Sent frame snapshot: frame_id=" + std::to_string(snapshot.frame_id));
}

void FrameSyncComponent::on_disconnect(const net::TcpConnectionPtr& conn) {
    uint32_t player_id = 0;
    std::string room_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(conn.get());
        if (it != connections_.end()) {
            player_id = it->second.player_id;
            room_id = it->second.room_id;
            connections_.erase(it);
        }
    }
    if (!room_id.empty()) {
        leave_room(player_id, room_id);
    }
}

service::SessionManager* FrameSyncComponent::get_session_manager() {
    return nullptr;
}

uint32_t FrameSyncComponent::get_player_id(const net::TcpConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connections_.find(conn.get());
    if (it != connections_.end()) {
        return it->second.player_id;
    }
    return 0;
}

std::string FrameSyncComponent::get_room_id(const net::TcpConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connections_.find(conn.get());
    if (it != connections_.end()) {
        return it->second.room_id;
    }
    return "";
}

} // namespace sync
} // namespace chwell
