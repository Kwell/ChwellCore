#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <vector>
#include <iostream>
#include <algorithm>

#include "chwell/service/service.h"
#include "chwell/service/session_manager.h"
#include "chwell/game/game_components.h"
#include "chwell/sync/frame_sync.h"
#include "chwell/core/logger.h"

using namespace chwell;

// ============================================
// SessionManager 集成测试
// ============================================

TEST(SessionManagerIntegrationTest, DISABLED_LoginLogoutAndQueryInterfaces) {
    service::SessionManager mgr;

    // 创建两个虚拟连接（使用静态对象地址）
    static int conn1_obj, conn2_obj;
    auto conn1 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn1_obj),
        [](net::TcpConnection*) {});
    auto conn2 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn2_obj),
        [](net::TcpConnection*) {});

    std::cout << "conn1.get() = " << conn1.get() << std::endl;
    std::cout << "conn2.get() = " << conn2.get() << std::endl;

    // 测试登录/登出
    std::cout << "Testing login status before login..." << std::endl;
    bool logged_in = mgr.is_logged_in(conn1);
    std::cout << "Logged in: " << (logged_in ? "true" : "false") << std::endl;
    EXPECT_FALSE(logged_in);

    std::cout << "Testing get_player_id before login..." << std::endl;
    std::string player_id = mgr.get_player_id(conn1);
    std::cout << "Player ID: '" << player_id << "'" << std::endl;
    EXPECT_TRUE(player_id.empty());

    std::cout << "Logging in..." << std::endl;
    mgr.login(conn1, "player123");
    std::cout << "Login done." << std::endl;

    std::cout << "Testing login status after login..." << std::endl;
    logged_in = mgr.is_logged_in(conn1);
    std::cout << "Logged in: " << (logged_in ? "true" : "false") << std::endl;
    EXPECT_TRUE(logged_in);

    std::cout << "Testing get_player_id after login..." << std::endl;
    player_id = mgr.get_player_id(conn1);
    std::cout << "Player ID: '" << player_id << "'" << std::endl;
    EXPECT_EQ("player123", player_id);

    std::cout << "Logging out..." << std::endl;
    mgr.logout(conn1);
    std::cout << "Logout done." << std::endl;

    std::cout << "Testing login status after logout..." << std::endl;
    logged_in = mgr.is_logged_in(conn1);
    std::cout << "Logged in: " << (logged_in ? "true" : "false") << std::endl;
    EXPECT_FALSE(logged_in);

    std::cout << "Testing get_player_id after logout..." << std::endl;
    player_id = mgr.get_player_id(conn1);
    std::cout << "Player ID: '" << player_id << "'" << std::endl;
    EXPECT_TRUE(player_id.empty());

    // 测试多玩家
    std::cout << "Testing multiple players..." << std::endl;
    mgr.login(conn1, "alice");
    mgr.login(conn2, "bob");

    EXPECT_TRUE(mgr.is_logged_in(conn1));
    EXPECT_TRUE(mgr.is_logged_in(conn2));
    EXPECT_EQ("alice", mgr.get_player_id(conn1));
    EXPECT_EQ("bob", mgr.get_player_id(conn2));
}

TEST(SessionManagerIntegrationTest, DISABLED_JoinLeaveRoomAndGetPlayersInRoom) {
    service::SessionManager mgr;

    // 创建两个虚拟连接
    static int conn1_obj, conn2_obj, conn3_obj;
    auto conn1 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn1_obj),
        [](net::TcpConnection*) {});
    auto conn2 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn2_obj),
        [](net::TcpConnection*) {});
    auto conn3 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn3_obj),
        [](net::TcpConnection*) {});

    // 登录并加入房间
    mgr.login(conn1, "alice");
    mgr.login(conn2, "bob");
    mgr.login(conn3, "charlie");

    mgr.join_room(conn1, "room1");
    mgr.join_room(conn2, "room1");
    mgr.join_room(conn3, "room2");

    // 测试房间查询
    auto players1 = mgr.get_players_in_room("room1");
    ASSERT_EQ(2u, players1.size());
    EXPECT_NE(players1.end(), std::find(players1.begin(), players1.end(), "alice"));
    EXPECT_NE(players1.end(), std::find(players1.begin(), players1.end(), "bob"));

    auto players2 = mgr.get_players_in_room("room2");
    ASSERT_EQ(1u, players2.size());
    EXPECT_EQ("charlie", players2[0]);

    // 测试离开房间
    mgr.leave_room(conn1);
    players1 = mgr.get_players_in_room("room1");
    ASSERT_EQ(1u, players1.size());
    EXPECT_EQ("bob", players1[0]);

    // 测试房间不存在
    auto players3 = mgr.get_players_in_room("room_not_exist");
    EXPECT_EQ(0u, players3.size());
}

// ============================================
// RoomComponent 集成测试
// ============================================

TEST(RoomComponentIntegrationTest, DISABLED_BasicOperations) {
    game::RoomComponent room_comp;

    // 创建虚拟连接
    static int conn1_obj, conn2_obj, conn3_obj;
    auto conn1 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn1_obj),
        [](net::TcpConnection*) {});
    auto conn2 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn2_obj),
        [](net::TcpConnection*) {});
    auto conn3 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn3_obj),
        [](net::TcpConnection*) {});

    // 创建两个房间
    room_comp.join_room(conn1, "room1");
    room_comp.join_room(conn2, "room1");
    room_comp.join_room(conn2, "room2");
    room_comp.join_room(conn3, "room1");

    // 测试获取房间内的连接
    auto room1_conns = room_comp.get_connections_in_room("room1");
    EXPECT_EQ(3u, room1_conns.size());

    auto room2_conns = room_comp.get_connections_in_room("room2");
    EXPECT_EQ(1u, room2_conns.size());

    // 测试离开房间
    room_comp.leave_room(conn1);

    room1_conns = room_comp.get_connections_in_room("room1");
    EXPECT_EQ(2u, room1_conns.size());

    // 测试连接断开时自动离开房间
    room_comp.on_disconnect(conn2);
    room1_conns = room_comp.get_connections_in_room("room1");
    EXPECT_EQ(1u, room1_conns.size());

    room2_conns = room_comp.get_connections_in_room("room2");
    EXPECT_EQ(0u, room2_conns.size());
}

// ============================================
// FrameSync 集成测试
// ============================================

TEST(FrameSyncIntegrationTest, DISABLED_FrameSyncRoomJoinLeave) {
    sync::FrameSyncRoom room("test_room", 30);

    // 创建虚拟连接
    static int conn1_obj, conn2_obj;
    auto conn1 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn1_obj),
        [](net::TcpConnection*) {});
    auto conn2 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn2_obj),
        [](net::TcpConnection*) {});

    // 测试加入/离开
    room.join_player(12345, conn1);
    EXPECT_EQ(1, room.player_count());

    room.join_player(67890, conn2);
    EXPECT_EQ(2, room.player_count());

    room.leave_player(12345);
    EXPECT_EQ(1, room.player_count());

    room.leave_player(67890);
    EXPECT_EQ(0, room.player_count());
}

TEST(FrameSyncIntegrationTest, DISABLED_FrameSyncRoomSubmitAndGetInputs) {
    sync::FrameSyncRoom room("test_room", 30);

    // 创建虚拟连接
    static int conn1_obj, conn2_obj;
    auto conn1 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn1_obj),
        [](net::TcpConnection*) {});
    auto conn2 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn2_obj),
        [](net::TcpConnection*) {});

    // 加入玩家
    room.join_player(12345, conn1);
    room.join_player(67890, conn2);

    // 提交输入
    sync::FrameInput input1;
    input1.frame_id = 10;
    input1.player_id = 12345;
    input1.input_data = {0x01, 0x02, 0x03};

    sync::FrameInput input2;
    input2.frame_id = 10;
    input2.player_id = 67890;
    input2.input_data = {0x04, 0x05, 0x06};

    room.submit_input(12345, input1);
    room.submit_input(67890, input2);

    // 获取输入
    auto inputs = room.get_all_inputs(10);
    EXPECT_EQ(2, inputs.size());
}

TEST(FrameSyncIntegrationTest, DISABLED_FrameSyncRoomMultipleInputs) {
    sync::FrameSyncRoom room("test_room", 30);

    // 创建虚拟连接
    static int conn1_obj, conn2_obj;
    auto conn1 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn1_obj),
        [](net::TcpConnection*) {});
    auto conn2 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn2_obj),
        [](net::TcpConnection*) {});

    room.join_player(12345, conn1);
    room.join_player(67890, conn2);

    // 提交多帧输入
    for (int frame = 10; frame < 20; ++frame) {
        sync::FrameInput input1;
        input1.frame_id = frame;
        input1.player_id = 12345;
        input1.input_data = {0x01, 0x02, 0x03};
        room.submit_input(12345, input1);

        sync::FrameInput input2;
        input2.frame_id = frame;
        input2.player_id = 67890;
        input2.input_data = {0x04, 0x05, 0x06};
        room.submit_input(67890, input2);
    }

    // 验证每一帧的输入
    for (int frame = 10; frame < 20; ++frame) {
        auto inputs = room.get_all_inputs(frame);
        EXPECT_EQ(2, inputs.size());
    }
}

TEST(FrameSyncIntegrationTest, DISABLED_FrameSyncRoomGetPlayerIds) {
    sync::FrameSyncRoom room("test_room", 30);

    // 创建虚拟连接
    static int conn1_obj, conn2_obj, conn3_obj;
    auto conn1 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn1_obj),
        [](net::TcpConnection*) {});
    auto conn2 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn2_obj),
        [](net::TcpConnection*) {});
    auto conn3 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn3_obj),
        [](net::TcpConnection*) {});

    room.join_player(12345, conn1);
    room.join_player(67890, conn2);
    room.join_player(11111, conn3);

    // 获取玩家 ID 列表
    auto player_ids = room.get_player_ids();
    EXPECT_EQ(3, player_ids.size());

    // 离开一个玩家
    room.leave_player(12345);

    player_ids = room.get_player_ids();
    EXPECT_EQ(2, player_ids.size());

    // 空房间
    room.leave_player(67890);
    room.leave_player(11111);

    player_ids = room.get_player_ids();
    EXPECT_EQ(0, player_ids.size());
}

TEST(FrameSyncIntegrationTest, DISABLED_FrameSyncRoomDifferentFrameInputs) {
    sync::FrameSyncRoom room("test_room", 30);

    // 创建虚拟连接
    static int conn1_obj, conn2_obj;
    auto conn1 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn1_obj),
        [](net::TcpConnection*) {});
    auto conn2 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn2_obj),
        [](net::TcpConnection*) {});

    room.join_player(12345, conn1);
    room.join_player(67890, conn2);

    // 玩家1 提交第10帧输入
    sync::FrameInput input1;
    input1.frame_id = 10;
    input1.player_id = 12345;
    input1.input_data = {0x01, 0x02, 0x03};
    room.submit_input(12345, input1);

    // 玩家2 提交第11帧输入
    sync::FrameInput input2;
    input2.frame_id = 11;
    input2.player_id = 67890;
    input2.input_data = {0x04, 0x05, 0x06};
    room.submit_input(67890, input2);

    // 验证第10帧只有1个输入
    auto inputs10 = room.get_all_inputs(10);
    EXPECT_EQ(1, inputs10.size());

    // 验证第11帧只有1个输入
    auto inputs11 = room.get_all_inputs(11);
    EXPECT_EQ(1, inputs11.size());
}

TEST(FrameSyncIntegrationTest, DISABLED_FrameSyncRoomAllInputsReady) {
    sync::FrameSyncRoom room("test_room", 30);

    // 创建虚拟连接
    static int conn1_obj, conn2_obj;
    auto conn1 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn1_obj),
        [](net::TcpConnection*) {});
    auto conn2 = net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(&conn2_obj),
        [](net::TcpConnection*) {});

    room.join_player(12345, conn1);
    room.join_player(67890, conn2);

    // 只有玩家1提交输入
    sync::FrameInput input1;
    input1.frame_id = 10;
    input1.player_id = 12345;
    input1.input_data = {0x01, 0x02, 0x03};
    room.submit_input(12345, input1);

    // 检查输入是否准备好
    auto inputs = room.get_all_inputs(10);
    EXPECT_EQ(1, inputs.size());

    // 玩家2也提交输入
    sync::FrameInput input2;
    input2.frame_id = 10;
    input2.player_id = 67890;
    input2.input_data = {0x04, 0x05, 0x06};
    room.submit_input(67890, input2);

    // 现在应该有2个输入
    inputs = room.get_all_inputs(10);
    EXPECT_EQ(2, inputs.size());
}
