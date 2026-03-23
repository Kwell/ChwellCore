#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <vector>

#include "chwell/service/session_manager.h"
#include "chwell/net/tcp_connection.h"

using namespace chwell;

namespace {

// 使用 aliasing 构造函数创建虚拟连接：避免触发 enable_shared_from_this 初始化
// aliasing ctor 不会访问 T 对象内部，因此对 reinterpret_cast 指针是安全的
static int dummy_conn1_obj;
static int dummy_conn2_obj;

net::TcpConnectionPtr make_dummy_conn(std::uintptr_t tag) {
    // guard 负责控制生命周期，ptr 仅作为 map key 使用（永不解引用）
    auto guard = std::make_shared<int>(static_cast<int>(tag));
    void* ptr = (tag == 1) ? static_cast<void*>(&dummy_conn1_obj)
                            : static_cast<void*>(&dummy_conn2_obj);
    return net::TcpConnectionPtr(guard, reinterpret_cast<net::TcpConnection*>(ptr));
}

}  // namespace

TEST(SessionManagerTest, LoginLogoutAndQueryInterfaces) {
    service::SessionManager mgr;

    auto conn = make_dummy_conn(1);

    EXPECT_FALSE(mgr.is_logged_in(conn));
    EXPECT_TRUE(mgr.get_player_id(conn).empty());

    mgr.login(conn, "player123");
    EXPECT_TRUE(mgr.is_logged_in(conn));
    EXPECT_EQ("player123", mgr.get_player_id(conn));

    mgr.logout(conn);
    EXPECT_FALSE(mgr.is_logged_in(conn));
    EXPECT_TRUE(mgr.get_player_id(conn).empty());
}

TEST(SessionManagerTest, JoinLeaveRoomAndGetPlayersInRoom) {
    service::SessionManager mgr;

    auto conn1 = make_dummy_conn(1);
    auto conn2 = make_dummy_conn(2);

    mgr.login(conn1, "alice");
    mgr.login(conn2, "bob");

    mgr.join_room(conn1, "room1");
    mgr.join_room(conn2, "room1");

    auto players = mgr.get_players_in_room("room1");
    ASSERT_EQ(2u, players.size());
    EXPECT_NE(players.end(), std::find(players.begin(), players.end(), "alice"));
    EXPECT_NE(players.end(), std::find(players.begin(), players.end(), "bob"));

    mgr.leave_room(conn1);
    players = mgr.get_players_in_room("room1");
    ASSERT_EQ(1u, players.size());
    EXPECT_EQ("bob", players[0]);
}

