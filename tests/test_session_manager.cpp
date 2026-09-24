#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <vector>

#include "chwell/service/session_manager.h"
#include "chwell/net/tcp_connection.h"

using namespace chwell;

namespace {

net::TcpConnectionPtr make_dummy_conn(std::uintptr_t tag) {
    (void)tag;
    // 必须是真正的 TcpConnection：SessionManager 等会调用 conn_id() 读成员，
    // aliasing/reinterpret 到 int 上会构成 global-buffer-overflow。
    // 每个实例自动生成唯一 conn_id_。
    return std::make_shared<net::TcpConnection>(net::TcpSocket());
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

