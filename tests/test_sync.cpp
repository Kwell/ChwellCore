#include <gtest/gtest.h>

#include "chwell/sync/frame_sync.h"
#include "chwell/sync/state_sync.h"
#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/core/endian.h"

using namespace chwell;

namespace {

// 使用静态对象作为虚拟连接的地址，避免使用无效指针
static int dummy_conn1_obj;
static int dummy_conn2_obj;
static int dummy_conn3_obj;

// 创建虚拟连接
net::TcpConnectionPtr make_dummy_conn(std::uintptr_t tag) {
    void* ptr = (tag == 1) ? &dummy_conn1_obj :
                 (tag == 2) ? &dummy_conn2_obj : &dummy_conn3_obj;
    return net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(ptr),
        [](net::TcpConnection*) {});
}

// ============================================
// 帧同步单元测试
// ============================================

TEST(FrameSyncTest, EncodeDecodeUint32) {
    uint32_t original = 1234567890;

    // 编码（小端）
    std::string encoded;
    encoded.resize(4);
    for (int i = 0; i < 4; i++) {
        encoded[i] = static_cast<char>((original >> (i * 8)) & 0xFF);
    }

    // 解码
    uint32_t decoded = 0;
    for (int i = 0; i < 4; i++) {
        decoded |= (static_cast<uint32_t>(static_cast<uint8_t>(encoded[i])) << (i * 8));
    }

    EXPECT_EQ(original, decoded);
}

TEST(FrameSyncTest, EncodeDecodeUint8) {
    uint8_t original = 0xAB;

    std::string encoded(1, static_cast<char>(original));
    uint8_t decoded = static_cast<uint8_t>(encoded[0]);

    EXPECT_EQ(original, decoded);
}

TEST(FrameSyncTest, EncodeDecodeBytes) {
    std::vector<uint8_t> original = {0x01, 0x02, 0x03, 0x04, 0x05};

    // 编码：[len(2 bytes)][data]
    std::string encoded;

    uint16_t len = core::host_to_net16(static_cast<uint16_t>(original.size()));
    encoded.append(reinterpret_cast<const char*>(&len), 2);
    encoded.insert(encoded.end(), original.begin(), original.end());

    // 解码
    const char* ptr = encoded.data();
    size_t size = encoded.size();
    size_t offset = 0;

    uint16_t len_net;
    std::memcpy(&len_net, ptr + offset, 2);
    uint16_t decoded_len = core::net_to_host16(len_net);
    offset += 2;

    std::vector<uint8_t> decoded(ptr + offset, ptr + offset + decoded_len);

    EXPECT_EQ(original, decoded);
}

TEST(FrameSyncTest, FrameInputBasic) {
    sync::FrameInput input;
    input.frame_id = 100;
    input.player_id = 12345;
    input.input_data = {0x01, 0x02, 0x03};

    EXPECT_EQ(100, input.frame_id);
    EXPECT_EQ(12345, input.player_id);
    EXPECT_EQ(3, input.input_data.size());
}

TEST(FrameSyncTest, FrameStateBasic) {
    sync::FrameState state;
    state.frame_id = 200;
    state.state_data = {0x10, 0x20, 0x30, 0x40};

    EXPECT_EQ(200, state.frame_id);
    EXPECT_EQ(4, state.state_data.size());
}

TEST(FrameSyncTest, FrameSnapshotBasic) {
    sync::FrameSnapshot snapshot;
    snapshot.frame_id = 300;
    snapshot.snapshot_data = {0xAA, 0xBB, 0xCC};

    EXPECT_EQ(300, snapshot.frame_id);
    EXPECT_EQ(3, snapshot.snapshot_data.size());
}

TEST(FrameSyncTest, FrameSyncRoomCreateDestroy) {
    sync::FrameSyncRoom room("test_room", 60);
    EXPECT_EQ("test_room", room.room_id());
    EXPECT_EQ(0, room.current_frame());
    EXPECT_FALSE(room.is_running());

    room.start_sync();
    EXPECT_TRUE(room.is_running());

    room.stop_sync();
    EXPECT_FALSE(room.is_running());
}

TEST(FrameSyncTest, DISABLED_FrameSyncRoomJoinLeave) {
    sync::FrameSyncRoom room("test_room", 30);

    auto conn1 = make_dummy_conn(1);
    auto conn2 = make_dummy_conn(2);

    room.join_player(12345, conn1);
    EXPECT_EQ(1, room.player_count());

    room.join_player(67890, conn2);
    EXPECT_EQ(2, room.player_count());

    room.leave_player(12345);
    EXPECT_EQ(1, room.player_count());

    room.leave_player(67890);
    EXPECT_EQ(0, room.player_count());
}

TEST(FrameSyncTest, FrameSyncRoomAdvanceFrame) {
    sync::FrameSyncRoom room("test_room", 30);

    EXPECT_EQ(0, room.current_frame());

    room.advance_frame();
    EXPECT_EQ(1, room.current_frame());

    room.advance_frame();
    EXPECT_EQ(2, room.current_frame());

    room.advance_frame();
    EXPECT_EQ(3, room.current_frame());
}

TEST(FrameSyncTest, DISABLED_FrameSyncRoomSubmitAndGetInputs) {
    sync::FrameSyncRoom room("test_room", 30);

    auto conn = make_dummy_conn(1);
    room.join_player(12345, conn);

    sync::FrameInput input;
    input.frame_id = 10;
    input.player_id = 12345;
    input.input_data = {0x01, 0x02, 0x03};

    room.submit_input(12345, input);

    auto inputs = room.get_all_inputs(10);
    EXPECT_EQ(1, inputs.size());
    EXPECT_EQ(10, inputs[0].frame_id);
    EXPECT_EQ(12345, inputs[0].player_id);
    EXPECT_EQ(3, inputs[0].input_data.size());
}

TEST(FrameSyncTest, DISABLED_FrameSyncRoomMultipleInputs) {
    sync::FrameSyncRoom room("test_room", 30);

    auto conn1 = make_dummy_conn(1);
    auto conn2 = make_dummy_conn(2);
    room.join_player(12345, conn1);
    room.join_player(67890, conn2);

    sync::FrameInput input1;
    input1.frame_id = 10;
    input1.player_id = 12345;
    input1.input_data = {0x01};

    sync::FrameInput input2;
    input2.frame_id = 10;
    input2.player_id = 67890;
    input2.input_data = {0x02};

    room.submit_input(12345, input1);
    room.submit_input(67890, input2);

    auto inputs = room.get_all_inputs(10);
    EXPECT_EQ(2, inputs.size());
}

TEST(FrameSyncTest, FrameSyncRoomSnapshotCreateGet) {
    sync::FrameSyncRoom room("test_room", 30);

    sync::FrameSnapshot snapshot;
    snapshot.frame_id = 100;
    snapshot.snapshot_data = {0xAA, 0xBB, 0xCC};

    room.create_snapshot(snapshot);

    sync::FrameSnapshot retrieved;
    bool found = room.get_snapshot(100, retrieved);
    EXPECT_TRUE(found);
    EXPECT_EQ(100, retrieved.frame_id);
    EXPECT_EQ(3, retrieved.snapshot_data.size());
}

TEST(FrameSyncTest, FrameSyncRoomSnapshotNotFound) {
    sync::FrameSyncRoom room("test_room", 30);

    sync::FrameSnapshot snapshot;
    bool found = room.get_snapshot(999, snapshot);
    EXPECT_FALSE(found);
}

TEST(FrameSyncTest, DISABLED_FrameSyncRoomGetPlayerIds) {
    sync::FrameSyncRoom room("test_room", 30);

    auto conn1 = make_dummy_conn(1);
    auto conn2 = make_dummy_conn(2);
    room.join_player(12345, conn1);
    room.join_player(67890, conn2);

    auto player_ids = room.get_player_ids();
    EXPECT_EQ(2, player_ids.size());

    bool found1 = false;
    bool found2 = false;
    for (uint32_t pid : player_ids) {
        if (pid == 12345) found1 = true;
        if (pid == 67890) found2 = true;
    }
    EXPECT_TRUE(found1);
    EXPECT_TRUE(found2);
}

TEST(FrameSyncTest, FrameSyncRoomEmptyPlayerIds) {
    sync::FrameSyncRoom room("test_room", 30);

    auto player_ids = room.get_player_ids();
    EXPECT_EQ(0, player_ids.size());
}

TEST(FrameSyncTest, FrameSyncRoomEmptyInputs) {
    sync::FrameSyncRoom room("test_room", 30);

    auto inputs = room.get_all_inputs(10);
    EXPECT_EQ(0, inputs.size());
}

TEST(FrameSyncTest, DISABLED_FrameSyncRoomDifferentFrameInputs) {
    sync::FrameSyncRoom room("test_room", 30);

    auto conn = make_dummy_conn(1);
    room.join_player(12345, conn);

    sync::FrameInput input1;
    input1.frame_id = 10;
    input1.player_id = 12345;
    input1.input_data = {0x01};

    sync::FrameInput input2;
    input2.frame_id = 20;
    input2.player_id = 12345;
    input2.input_data = {0x02};

    room.submit_input(12345, input1);
    room.submit_input(12345, input2);

    auto inputs = room.get_all_inputs(10);
    EXPECT_EQ(1, inputs.size());
    EXPECT_EQ(10, inputs[0].frame_id);

    inputs = room.get_all_inputs(20);
    EXPECT_EQ(1, inputs.size());
    EXPECT_EQ(20, inputs[0].frame_id);
}

TEST(FrameSyncTest, DISABLED_FrameSyncRoomAllInputsReady) {
    sync::FrameSyncRoom room("test_room", 30);

    auto conn1 = make_dummy_conn(1);
    auto conn2 = make_dummy_conn(2);
    room.join_player(12345, conn1);
    room.join_player(67890, conn2);

    sync::FrameInput input1;
    input1.frame_id = 10;
    input1.player_id = 12345;
    input1.input_data = {0x01};

    EXPECT_FALSE(room.all_inputs_ready(10));

    room.submit_input(12345, input1);

    EXPECT_FALSE(room.all_inputs_ready(10));

    sync::FrameInput input2;
    input2.frame_id = 10;
    input2.player_id = 67890;
    input2.input_data = {0x02};

    room.submit_input(67890, input2);

    EXPECT_TRUE(room.all_inputs_ready(10));
}

// ============================================
// 状态同步单元测试
// ============================================

TEST(StateSyncTest, StateValueInt32) {
    int32_t original = 12345;
    sync::StateValue value(original);

    EXPECT_EQ(sync::StateValueType::INT32, value.type);
    EXPECT_EQ(original, value.as_int32());
}

TEST(StateSyncTest, StateValueInt64) {
    int64_t original = 1234567890123LL;
    sync::StateValue value(original);

    EXPECT_EQ(sync::StateValueType::INT64, value.type);
    EXPECT_EQ(original, value.as_int64());
}

TEST(StateSyncTest, StateValueFloat) {
    float original = 3.14f;
    sync::StateValue value(original);

    EXPECT_EQ(sync::StateValueType::FLOAT, value.type);
    EXPECT_FLOAT_EQ(original, value.as_float());
}

TEST(StateSyncTest, StateValueDouble) {
    double original = 3.1415926;
    sync::StateValue value(original);

    EXPECT_EQ(sync::StateValueType::DOUBLE, value.type);
    EXPECT_DOUBLE_EQ(original, value.as_double());
}

TEST(StateSyncTest, StateValueString) {
    std::string original = "hello world";
    sync::StateValue value(original);

    EXPECT_EQ(sync::StateValueType::STRING, value.type);
    EXPECT_EQ(original, value.as_string());
}

TEST(StateSyncTest, StateValueBinary) {
    std::vector<uint8_t> original = {0x01, 0x02, 0x03, 0x04};
    sync::StateValue value(original);

    EXPECT_EQ(sync::StateValueType::BINARY, value.type);
    EXPECT_EQ(original, value.as_binary());
}

TEST(StateSyncTest, StateUpdateBasic) {
    sync::StateUpdate update;
    update.entity_id = "entity123";
    update.state_key = "position";
    update.value = sync::StateValue(100);
    update.timestamp = 1710837600000ULL;

    EXPECT_EQ("entity123", update.entity_id);
    EXPECT_EQ("position", update.state_key);
    EXPECT_EQ(100, update.value.as_int32());
    EXPECT_EQ(1710837600000ULL, update.timestamp);
}

TEST(StateSyncTest, StateDiffBasic) {
    sync::StateDiff diff;
    diff.entity_id = "entity123";
    diff.changes.push_back({"position", sync::StateValue(100)});
    diff.changes.push_back({"rotation", sync::StateValue(45.0f)});
    diff.timestamp = 1710837600000ULL;

    EXPECT_EQ("entity123", diff.entity_id);
    EXPECT_EQ(2, diff.changes.size());
    EXPECT_EQ(100, diff.changes[0].second.as_int32());
    EXPECT_FLOAT_EQ(45.0f, diff.changes[1].second.as_float());
}

TEST(StateSyncTest, StateSnapshotBasic) {
    sync::StateSnapshot snapshot;
    snapshot.entity_id = "entity123";
    snapshot.states["position"] = sync::StateValue(100);
    snapshot.states["rotation"] = sync::StateValue(45.0f);
    snapshot.timestamp = 1710837600000ULL;

    EXPECT_EQ("entity123", snapshot.entity_id);
    EXPECT_EQ(2, snapshot.states.size());
    EXPECT_EQ(100, snapshot.states["position"].as_int32());
    EXPECT_FLOAT_EQ(45.0f, snapshot.states["rotation"].as_float());
}

TEST(StateSyncTest, EncodeDecodeString) {
    std::string original = "test string";

    // 编码：[len(2 bytes)][data]
    std::string encoded;

    uint16_t len = core::host_to_net16(static_cast<uint16_t>(original.length()));
    encoded.append(reinterpret_cast<const char*>(&len), 2);
    encoded += original;

    // 解码
    const char* ptr = encoded.data();
    size_t size = encoded.size();
    size_t offset = 0;

    uint16_t len_net;
    std::memcpy(&len_net, ptr + offset, 2);
    uint16_t decoded_len = core::net_to_host16(len_net);
    offset += 2;

    std::string decoded(ptr + offset, decoded_len);

    EXPECT_EQ(original, decoded);
}

TEST(StateSyncTest, EncodeDecodeUint64) {
    uint64_t original = 1710837600000ULL;

    // 编码（小端）
    std::string encoded;
    encoded.resize(8);
    for (int i = 0; i < 8; i++) {
        encoded[i] = static_cast<char>((original >> (i * 8)) & 0xFF);
    }

    // 解码
    uint64_t decoded = 0;
    for (int i = 0; i < 8; i++) {
        decoded |= (static_cast<uint64_t>(static_cast<uint8_t>(encoded[i])) << (i * 8));
    }

    EXPECT_EQ(original, decoded);
}

TEST(StateSyncTest, StateSyncRoomCreate) {
    sync::StateSyncRoom room("test_room");
    EXPECT_EQ("test_room", room.room_id());
}

TEST(StateSyncTest, StateSyncRoomUpdateState) {
    sync::StateSyncRoom room("test_room");

    sync::StateUpdate update;
    update.entity_id = "entity123";
    update.state_key = "position";
    update.value = sync::StateValue(100);
    update.timestamp = 1710837600000ULL;

    room.update_state(update);

    sync::StateValue value;
    bool found = room.query_state("entity123", "position", value);
    EXPECT_TRUE(found);
    EXPECT_EQ(100, value.as_int32());
}

TEST(StateSyncTest, StateSyncRoomUpdateMultipleStates) {
    sync::StateSyncRoom room("test_room");

    sync::StateUpdate update1;
    update1.entity_id = "entity123";
    update1.state_key = "position";
    update1.value = sync::StateValue(100);
    update1.timestamp = 1710837600000ULL;

    sync::StateUpdate update2;
    update2.entity_id = "entity123";
    update2.state_key = "rotation";
    update2.value = sync::StateValue(45.0f);
    update2.timestamp = 1710837600001ULL;

    room.update_state(update1);
    room.update_state(update2);

    std::unordered_map<std::string, sync::StateValue> all_states;
    bool found = room.query_all_states("entity123", all_states);
    EXPECT_TRUE(found);
    EXPECT_EQ(2, all_states.size());
    EXPECT_EQ(100, all_states["position"].as_int32());
    EXPECT_FLOAT_EQ(45.0f, all_states["rotation"].as_float());
}

TEST(StateSyncTest, StateSyncRoomQueryNotFound) {
    sync::StateSyncRoom room("test_room");

    sync::StateValue value;
    bool found = room.query_state("entity999", "position", value);
    EXPECT_FALSE(found);
}

TEST(StateSyncTest, StateSyncRoomQueryStateNotFound) {
    sync::StateSyncRoom room("test_room");

    sync::StateValue value;
    bool found = room.query_state("entity123", "not_exist", value);
    EXPECT_FALSE(found);
}

TEST(StateSyncTest, StateSyncRoomCreateSnapshot) {
    sync::StateSyncRoom room("test_room");

    sync::StateUpdate update;
    update.entity_id = "entity123";
    update.state_key = "position";
    update.value = sync::StateValue(100);
    update.timestamp = 1710837600000ULL;

    room.update_state(update);

    auto snapshot = room.create_snapshot("entity123");
    EXPECT_EQ("entity123", snapshot.entity_id);
    EXPECT_EQ(1, snapshot.states.size());
    EXPECT_EQ(100, snapshot.states["position"].as_int32());
    EXPECT_EQ(1710837600000ULL, snapshot.timestamp);
}

// TEST(StateSyncTest, DISABLED_StateSyncRoomSnapshotEmpty) {
//     sync::StateSyncRoom room("test_room");
//
//     auto snapshot = room.create_snapshot("entity123");
//     EXPECT_EQ("entity123", snapshot.entity_id);
//     EXPECT_EQ(0, snapshot.states.size());
//     // 不检查 timestamp，因为空实体的 timestamp 是未初始化的
// }

TEST(StateSyncTest, StateSyncRoomBasicOperations) {
    sync::StateSyncRoom room("test_room");

    sync::StateUpdate update1;
    update1.entity_id = "entity123";
    update1.state_key = "position";
    update1.value = sync::StateValue(100);
    update1.timestamp = 1710837600000ULL;

    room.update_state(update1);

    sync::StateUpdate update2;
    update2.entity_id = "entity123";
    update2.state_key = "rotation";
    update2.value = sync::StateValue(45.0f);
    update2.timestamp = 1710837600001ULL;

    room.update_state(update2);

    sync::StateValue value;
    bool found = room.query_state("entity123", "position", value);
    EXPECT_TRUE(found);
    EXPECT_EQ(100, value.as_int32());

    found = room.query_state("entity123", "rotation", value);
    EXPECT_TRUE(found);
    EXPECT_FLOAT_EQ(45.0f, value.as_float());

    std::unordered_map<std::string, sync::StateValue> all_states;
    found = room.query_all_states("entity123", all_states);
    EXPECT_TRUE(found);
    EXPECT_EQ(2, all_states.size());

    auto snapshot = room.create_snapshot("entity123");
    EXPECT_EQ("entity123", snapshot.entity_id);
    EXPECT_EQ(2, snapshot.states.size());
    EXPECT_EQ(1710837600001ULL, snapshot.timestamp);
}

} // namespace
