#include "chwell/sync/state_sync.h"
#include "chwell/service/protocol_router.h"
#include "chwell/protocol/message.h"
#include "chwell/core/endian.h"
#include <cstring>

namespace chwell {
namespace sync {

// ============================================
// 辅助函数：编码/解码
// ============================================

static bool decode_string(const char* data, size_t size, size_t& offset, std::string& out) {
    if (offset + 2 > size) return false;

    uint16_t len_net;
    std::memcpy(&len_net, data + offset, 2);
    uint16_t len = core::net_to_host16(len_net);
    offset += 2;

    if (offset + len > size) return false;
    out.assign(data + offset, len);
    offset += len;

    return true;
}

static std::string encode_string(const std::string& str) {
    std::string result;

    uint16_t len = core::host_to_net16(static_cast<uint16_t>(str.length()));
    result.append(reinterpret_cast<const char*>(&len), 2);
    result += str;

    return result;
}

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

static bool decode_uint64(const char* data, size_t size, size_t& offset, uint64_t& out) {
    if (offset + 8 > size) return false;

    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value |= (static_cast<uint64_t>(static_cast<uint8_t>(data[offset + i])) << (i * 8));
    }
    out = value;
    offset += 8;

    return true;
}

static std::string encode_uint64(uint64_t value) {
    std::string result;
    result.resize(8);
    for (int i = 0; i < 8; i++) {
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

static std::string encode_state_value(const StateValue& value) {
    std::string result;

    // value_len（2字节，网络字节序）
    uint16_t len = core::host_to_net16(static_cast<uint16_t>(value.value.size()));
    result.append(reinterpret_cast<const char*>(&len), 2);

    // value
    result += value.value;

    return result;
}

// ============================================
// StateSyncComponent
// ============================================

void StateSyncComponent::on_register(service::Service& svc) {
    service_ = &svc;

    auto* router = svc.get_component<service::ProtocolRouterComponent>();
    if (router) {
        router->register_handler(state_cmd::C2S_STATE_UPDATE,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                this->handle_state_update(conn, msg.body);
            });

        router->register_handler(state_cmd::C2S_STATE_QUERY,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                this->handle_state_query(conn, msg.body);
            });

        router->register_handler(state_cmd::C2S_STATE_SUBSCRIBE,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                this->handle_state_subscribe(conn, msg.body);
            });

        router->register_handler(state_cmd::C2S_STATE_UNSUBSCRIBE,
            [this](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
                this->handle_state_unsubscribe(conn, msg.body);
            });

        CHWELL_LOG_INFO("StateSyncComponent registered handlers");
    } else {
        CHWELL_LOG_WARN("ProtocolRouterComponent not found");
    }
}

void StateSyncComponent::handle_state_update(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    // 解析: [entity_id_len(2)][entity_id][state_key_len(2)][state_key][value_type(1)][value_len(2)][value][timestamp(8)]
    const char* ptr = data.data();
    size_t size = data.size();
    size_t offset = 0;

    std::string entity_id;
    std::string state_key;
    uint8_t value_type = 0;
    std::vector<uint8_t> value_data;
    uint64_t timestamp = 0;

    if (!decode_string(ptr, size, offset, entity_id)) {
        CHWELL_LOG_ERROR("Failed to decode entity_id");
        return;
    }

    if (!decode_string(ptr, size, offset, state_key)) {
        CHWELL_LOG_ERROR("Failed to decode state_key");
        return;
    }

    if (!decode_uint8(ptr, size, offset, value_type)) {
        CHWELL_LOG_ERROR("Failed to decode value_type");
        return;
    }

    if (!decode_bytes(ptr, size, offset, value_data)) {
        CHWELL_LOG_ERROR("Failed to decode value");
        return;
    }

    if (!decode_uint64(ptr, size, offset, timestamp)) {
        CHWELL_LOG_ERROR("Failed to decode timestamp");
        return;
    }

    CHWELL_LOG_INFO("State update: entity_id=" + entity_id +
                    ", state_key=" + state_key +
                    ", value_type=" + std::to_string(value_type) +
                    ", timestamp=" + std::to_string(timestamp));

    // 创建 StateValue
    StateValue state_value;
    state_value.type = static_cast<StateValueType>(value_type);
    state_value.value.assign(value_data.begin(), value_data.end());

    // 创建 StateUpdate
    StateUpdate update;
    update.entity_id = entity_id;
    update.state_key = state_key;
    update.value = state_value;
    update.timestamp = timestamp;

    // 更新状态
    std::string room_id = get_room_id(conn);
    if (!room_id.empty()) {
        update_state(room_id, update);
    } else {
        CHWELL_LOG_WARN("Connection not in any room");
    }
}

void StateSyncComponent::handle_state_query(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    // 解析: [entity_id_len(2)][entity_id][state_key_len(2)][state_key]
    const char* ptr = data.data();
    size_t size = data.size();
    size_t offset = 0;

    std::string entity_id;
    std::string state_key;

    if (!decode_string(ptr, size, offset, entity_id)) {
        CHWELL_LOG_ERROR("Failed to decode entity_id");
        return;
    }

    if (!decode_string(ptr, size, offset, state_key)) {
        CHWELL_LOG_ERROR("Failed to decode state_key");
        return;
    }

    CHWELL_LOG_INFO("State query: entity_id=" + entity_id + ", state_key=" + state_key);

    // 查询状态
    std::string room_id = get_room_id(conn);
    StateValue state_value;
    if (!room_id.empty() && query_state(room_id, entity_id, state_key, state_value)) {
        // 发送状态更新响应
        // [entity_id_len(2)][entity_id][state_key_len(2)][state_key][value_type(1)][value_len(2)][value][timestamp(8)]
        std::string body;
        body += encode_string(entity_id);
        body += encode_string(state_key);
        body += encode_uint8(static_cast<uint8_t>(state_value.type));
        body += encode_bytes(std::vector<uint8_t>(state_value.value.begin(), state_value.value.end()));
        body += encode_uint64(0); // timestamp

        protocol::Message msg(state_cmd::S2C_STATE_UPDATE, std::vector<char>(body.begin(), body.end()));
        service::ProtocolRouterComponent::send_message(conn, msg);

        CHWELL_LOG_INFO("Sent state update response");
    } else {
        CHWELL_LOG_WARN("State not found");
    }
}

void StateSyncComponent::handle_state_subscribe(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    // 解析: [entity_id_len(2)][entity_id]
    const char* ptr = data.data();
    size_t size = data.size();
    size_t offset = 0;

    std::string entity_id;

    if (!decode_string(ptr, size, offset, entity_id)) {
        CHWELL_LOG_ERROR("Failed to decode entity_id");
        return;
    }

    CHWELL_LOG_INFO("State subscribe: entity_id=" + entity_id);

    // 订阅状态更新
    std::string room_id = get_room_id(conn);
    if (!room_id.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = rooms_.find(room_id);
        if (it != rooms_.end()) {
            it->second->subscribe(entity_id, conn);
            CHWELL_LOG_INFO("Subscribed to entity: " + entity_id);
        }
    }
}

void StateSyncComponent::handle_state_unsubscribe(const net::TcpConnectionPtr& conn, const std::vector<char>& data) {
    // 解析: [entity_id_len(2)][entity_id]
    const char* ptr = data.data();
    size_t size = data.size();
    size_t offset = 0;

    std::string entity_id;

    if (!decode_string(ptr, size, offset, entity_id)) {
        CHWELL_LOG_ERROR("Failed to decode entity_id");
        return;
    }

    CHWELL_LOG_INFO("State unsubscribe: entity_id=" + entity_id);

    // 取消订阅
    std::string room_id = get_room_id(conn);
    if (!room_id.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = rooms_.find(room_id);
        if (it != rooms_.end()) {
            it->second->unsubscribe(entity_id, conn);
            CHWELL_LOG_INFO("Unsubscribed from entity: " + entity_id);
        }
    }
}

void StateSyncComponent::create_room(const std::string& room_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (rooms_.find(room_id) != rooms_.end()) {
        CHWELL_LOG_WARN("Room already exists: " + room_id);
        return;
    }

    auto room = std::make_shared<StateSyncRoom>(room_id);

    // 设置回调函数
    room->set_diff_callback([this](const net::TcpConnectionPtr& conn, const StateDiff& diff) {
        this->send_state_diff(conn, diff);
    });

    room->set_snapshot_callback([this](const net::TcpConnectionPtr& conn, const StateSnapshot& snapshot) {
        this->send_state_snapshot(conn, snapshot);
    });

    rooms_[room_id] = room;

    CHWELL_LOG_INFO("Created state sync room: " + room_id);
}

void StateSyncComponent::destroy_room(const std::string& room_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = rooms_.find(room_id);
    if (it != rooms_.end()) {
        rooms_.erase(it);
        CHWELL_LOG_INFO("Destroyed state sync room: " + room_id);
    }
}

void StateSyncComponent::update_state(const std::string& room_id, const StateUpdate& update) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = rooms_.find(room_id);
    if (it != rooms_.end()) {
        it->second->update_state(update);
    }
}

bool StateSyncComponent::query_state(const std::string& room_id, const std::string& entity_id, const std::string& state_key, StateValue& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = rooms_.find(room_id);
    if (it != rooms_.end()) {
        return it->second->query_state(entity_id, state_key, out);
    }

    return false;
}

bool StateSyncComponent::query_all_states(const std::string& room_id, const std::string& entity_id, std::unordered_map<std::string, StateValue>& out) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = rooms_.find(room_id);
    if (it != rooms_.end()) {
        return it->second->query_all_states(entity_id, out);
    }

    return false;
}

StateSnapshot StateSyncComponent::create_snapshot(const std::string& room_id, const std::string& entity_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = rooms_.find(room_id);
    if (it != rooms_.end()) {
        return it->second->create_snapshot(entity_id);
    }

    StateSnapshot snapshot;
    return snapshot;
}

void StateSyncComponent::on_disconnect(const net::TcpConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = connections_.find(conn.get());
    if (it != connections_.end()) {
        std::string room_id = it->second;
        connections_.erase(it);

        // 从房间中移除所有订阅
        auto room_it = rooms_.find(room_id);
        if (room_it != rooms_.end()) {
            // 这里需要遍历所有实体，移除该连接的订阅
            // 暂时简化处理
        }
    }
}

service::SessionManager* StateSyncComponent::get_session_manager() {
    // 需要通过 Service 获取 SessionManager
    // 这里暂时返回 nullptr，实际使用时需要从 Service 获取
    return nullptr;
}

std::string StateSyncComponent::get_room_id(const net::TcpConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = connections_.find(conn.get());
    if (it != connections_.end()) {
        return it->second;
    }

    return "";
}

void StateSyncComponent::send_state_diff(const net::TcpConnectionPtr& conn, const StateDiff& diff) {
    // 编码: [entity_id_len][entity_id][change_count][change1_key_len][change1_key][change1_type][change1_len][change1_value][timestamp(8 bytes)]
    std::string body;

    // entity_id
    body += encode_string(diff.entity_id);

    // change_count（2字节，网络字节序）
    uint16_t count = core::host_to_net16(static_cast<uint16_t>(diff.changes.size()));
    body.append(reinterpret_cast<const char*>(&count), 2);

    // changes
    for (const auto& change : diff.changes) {
        // key
        body += encode_string(change.first);

        // type（1字节）
        uint8_t type = static_cast<uint8_t>(change.second.type);
        body.append(reinterpret_cast<const char*>(&type), 1);

        // value
        body += encode_state_value(change.second);
    }

    // timestamp（8字节，小端）
    body += encode_uint64(diff.timestamp);

    protocol::Message msg(state_cmd::S2C_STATE_DIFF, std::vector<char>(body.begin(), body.end()));
    service::ProtocolRouterComponent::send_message(conn, msg);

    CHWELL_LOG_INFO("Sent state diff: entity_id=" + diff.entity_id + ", changes=" + std::to_string(diff.changes.size()));
}

void StateSyncComponent::send_state_snapshot(const net::TcpConnectionPtr& conn, const StateSnapshot& snapshot) {
    // 编码: [entity_id_len][entity_id][state_count][state1_key_len][state1_key][state1_type][state1_len][state1_value][timestamp(8 bytes)]
    std::string body;

    // entity_id
    body += encode_string(snapshot.entity_id);

    // state_count（2字节，网络字节序）
    uint16_t count = core::host_to_net16(static_cast<uint16_t>(snapshot.states.size()));
    body.append(reinterpret_cast<const char*>(&count), 2);

    // states
    for (const auto& pair : snapshot.states) {
        // key
        body += encode_string(pair.first);

        // type（1字节）
        uint8_t type = static_cast<uint8_t>(pair.second.type);
        body.append(reinterpret_cast<const char*>(&type), 1);

        // value
        body += encode_state_value(pair.second);
    }

    // timestamp（8字节，小端）
    body += encode_uint64(snapshot.timestamp);

    protocol::Message msg(state_cmd::S2C_STATE_SNAPSHOT, std::vector<char>(body.begin(), body.end()));
    service::ProtocolRouterComponent::send_message(conn, msg);

    CHWELL_LOG_INFO("Sent state snapshot: entity_id=" + snapshot.entity_id + ", states=" + std::to_string(snapshot.states.size()));
}

} // namespace sync
} // namespace chwell
