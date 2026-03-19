#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <vector>

#include "chwell/service/session_manager.h"
#include "chwell/net/tcp_connection.h"

using namespace chwell;

namespace {

// 使用静态对象作为虚拟连接的地址，避免使用无效指针
static int dummy_conn1_obj;
static int dummy_conn2_obj;

net::TcpConnectionPtr make_dummy_conn(std::uintptr_t tag) {
    // 使用静态对象的地址作为指针值
    void* ptr = (tag == 1) ? &dummy_conn1_obj : &dummy_conn2_obj;
    return net::TcpConnectionPtr(
        reinterpret_cast<net::TcpConnection*>(ptr),
        [](net::TcpConnection*) {});
}

}  // namespace

TEST(SessionManagerTest, DISABLED_LoginLogoutAndQueryInterfaces) {
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

TEST(SessionManagerTest, DISABLED_JoinLeaveRoomAndGetPlayersInRoom) {
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

