#include <csignal>
#include <iostream>
#include <string>
#include <sstream>
#include <algorithm>
#include <map>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

#include "chwell/core/logger.h"
#include "chwell/core/config.h"
#include "chwell/service/service.h"
#include "chwell/service/protocol_router.h"
#include "chwell/service/session_manager.h"
#include "chwell/protocol/message.h"
#include "game_commands.h"

using namespace chwell;

struct Room {
    std::string room_id;
    std::string name;
    net::TcpConnectionPtr owner_conn;
    std::vector<net::TcpConnectionPtr> members;
    int state{0};  // 0=waiting, 1=playing
    int hp[2]{100, 100};
    int turn{0};
    int round{0};
    std::string result;  // "p1_win"/"p2_win"/""
};

class GameRoomComponent : public service::Component {
public:
    virtual std::string name() const override { return "GameRoomComponent"; }

    virtual void on_disconnect(const net::TcpConnectionPtr& conn) override {
        leave_room_internal(conn);
    }

    void leave_room_internal(const net::TcpConnectionPtr& conn) {
        auto it = conn_to_room_.find(conn.get());
        if (it == conn_to_room_.end()) return;
        std::string rid = it->second;
        conn_to_room_.erase(it);

        auto rit = rooms_.find(rid);
        if (rit == rooms_.end()) return;
        Room& r = rit->second;
        auto& m = r.members;
        m.erase(std::remove(m.begin(), m.end(), conn), m.end());
        if (m.empty()) {
            rooms_.erase(rit);
            return;
        }
        if (r.owner_conn.get() == conn.get()) {
            r.owner_conn = m[0];
        }
        if (r.state == 1) {
            r.state = 0;
            r.result = "opponent_left";
        }
        broadcast_room_info(rid);
    }

    std::string gen_room_id() {
        return "r" + std::to_string(static_cast<unsigned>(std::time(nullptr)) * 1000 + (rand() % 1000));
    }

    void broadcast_room_info(const std::string& room_id) {
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) return;
        Room& r = it->second;
        std::string body = room_info_json(r);
        protocol::Message msg(GameCmd::ROOM_INFO, body);
        for (const auto& c : r.members) {
            service::ProtocolRouterComponent::send_message(c, msg);
        }
    }

    void broadcast_game_state(const std::string& room_id) {
        auto it = rooms_.find(room_id);
        if (it == rooms_.end()) return;
        Room& r = it->second;
        std::string body = game_state_json(r);
        protocol::Message msg(GameCmd::GAME_STATE, body);
        for (const auto& c : r.members) {
            service::ProtocolRouterComponent::send_message(c, msg);
        }
    }

    static std::string room_info_json(const Room& r) {
        std::ostringstream o;
        o << "{\"roomId\":\"" << r.room_id << "\",\"name\":\"" << r.name << "\",\"players\":" << r.members.size()
          << ",\"state\":" << r.state << ",\"result\":\"" << r.result << "\"}";
        return o.str();
    }

    static std::string game_state_json(const Room& r) {
        std::ostringstream o;
        o << "{\"hp\":[" << r.hp[0] << "," << r.hp[1] << "],\"turn\":" << r.turn << ",\"round\":" << r.round
          << ",\"result\":\"" << r.result << "\"}";
        return o.str();
    }

    std::map<std::string, Room> rooms_;
    std::map<const net::TcpConnection*, std::string> conn_to_room_;
};

int main() {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    CHWELL_LOG_INFO("Starting Game Server...");

    core::Config cfg;
    cfg.load_from_file("server.conf");

    service::Service svc(static_cast<unsigned short>(cfg.listen_port()),
                        static_cast<std::size_t>(cfg.worker_threads()));

    auto* router = svc.add_component<service::ProtocolRouterComponent>();
    auto* session = svc.add_component<service::SessionManager>();
    auto* rooms = svc.add_component<GameRoomComponent>();

    // LOGIN
    router->register_handler(GameCmd::LOGIN,
        [session](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
            std::string player_id(msg.body.begin(), msg.body.end());
            if (player_id.empty()) {
                protocol::Message reply(GameCmd::LOGIN, "login failed: empty player_id");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            session->login(conn, player_id);
            protocol::Message reply(GameCmd::LOGIN, "login ok: " + player_id);
            service::ProtocolRouterComponent::send_message(conn, reply);
        });

    router->register_handler(GameCmd::LOGOUT,
        [session, rooms](const net::TcpConnectionPtr& conn, const protocol::Message&) {
            if (!session->is_logged_in(conn)) {
                protocol::Message reply(GameCmd::LOGOUT, "not logged in");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            rooms->leave_room_internal(conn);
            std::string pid = session->get_player_id(conn);
            session->logout(conn);
            protocol::Message reply(GameCmd::LOGOUT, "logout ok: " + pid);
            service::ProtocolRouterComponent::send_message(conn, reply);
        });

    router->register_handler(GameCmd::HEARTBEAT,
        [](const net::TcpConnectionPtr& conn, const protocol::Message&) {
            protocol::Message reply(GameCmd::HEARTBEAT, "pong");
            service::ProtocolRouterComponent::send_message(conn, reply);
        });

    // LIST_ROOMS
    router->register_handler(GameCmd::LIST_ROOMS,
        [session, rooms](const net::TcpConnectionPtr& conn, const protocol::Message&) {
            if (!session->is_logged_in(conn)) {
                protocol::Message reply(GameCmd::LIST_ROOMS, "[]");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            std::ostringstream o;
            o << "[";
            bool first = true;
            for (const auto& p : rooms->rooms_) {
                const Room& r = p.second;
                if (r.state != 0 || r.members.size() >= 2) continue;
                if (!first) o << ",";
                first = false;
                o << "{\"id\":\"" << r.room_id << "\",\"name\":\"" << r.name << "\",\"players\":"
                  << r.members.size() << ",\"state\":" << r.state << "}";
            }
            o << "]";
            protocol::Message reply(GameCmd::LIST_ROOMS, o.str());
            service::ProtocolRouterComponent::send_message(conn, reply);
        });

    // CREATE_ROOM
    router->register_handler(GameCmd::CREATE_ROOM,
        [session, rooms](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
            if (!session->is_logged_in(conn)) {
                protocol::Message reply(GameCmd::CREATE_ROOM, "err:login");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            auto it = rooms->conn_to_room_.find(conn.get());
            if (it != rooms->conn_to_room_.end()) {
                protocol::Message reply(GameCmd::CREATE_ROOM, "err:in_room");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            std::string name(msg.body.begin(), msg.body.end());
            std::string rid = rooms->gen_room_id();
            Room r;
            r.room_id = rid;
            r.name = name.empty() ? "room" : name;
            r.owner_conn = conn;
            r.members.push_back(conn);
            rooms->rooms_[rid] = r;
            rooms->conn_to_room_[conn.get()] = rid;
            protocol::Message reply(GameCmd::CREATE_ROOM, rid);
            service::ProtocolRouterComponent::send_message(conn, reply);
            rooms->broadcast_room_info(rid);
        });

    // JOIN_ROOM
    router->register_handler(GameCmd::JOIN_ROOM,
        [session, rooms](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
            if (!session->is_logged_in(conn)) {
                protocol::Message reply(GameCmd::JOIN_ROOM, "err:login");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            std::string rid(msg.body.begin(), msg.body.end());
            while (!rid.empty() && (rid.back() == '\r' || rid.back() == '\n')) rid.pop_back();
            auto rit = rooms->rooms_.find(rid);
            if (rit == rooms->rooms_.end()) {
                protocol::Message reply(GameCmd::JOIN_ROOM, "err:no_room");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            Room& r = rit->second;
            if (r.state != 0 || r.members.size() >= 2) {
                protocol::Message reply(GameCmd::JOIN_ROOM, "err:full");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            if (rooms->conn_to_room_.count(conn.get())) {
                protocol::Message reply(GameCmd::JOIN_ROOM, "err:in_room");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            r.members.push_back(conn);
            rooms->conn_to_room_[conn.get()] = rid;
            protocol::Message reply(GameCmd::JOIN_ROOM, "ok");
            service::ProtocolRouterComponent::send_message(conn, reply);
            rooms->broadcast_room_info(rid);
        });

    // LEAVE_ROOM
    router->register_handler(GameCmd::LEAVE_ROOM,
        [session, rooms](const net::TcpConnectionPtr& conn, const protocol::Message&) {
            if (!session->is_logged_in(conn)) {
                protocol::Message reply(GameCmd::LEAVE_ROOM, "err:login");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            rooms->leave_room_internal(conn);
            protocol::Message reply(GameCmd::LEAVE_ROOM, "ok");
            service::ProtocolRouterComponent::send_message(conn, reply);
        });

    // START_GAME
    router->register_handler(GameCmd::START_GAME,
        [session, rooms](const net::TcpConnectionPtr& conn, const protocol::Message&) {
            if (!session->is_logged_in(conn)) {
                protocol::Message reply(GameCmd::START_GAME, "err:login");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            auto it = rooms->conn_to_room_.find(conn.get());
            if (it == rooms->conn_to_room_.end()) {
                protocol::Message reply(GameCmd::START_GAME, "err:no_room");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            Room& r = rooms->rooms_[it->second];
            if (r.owner_conn.get() != conn.get()) {
                protocol::Message reply(GameCmd::START_GAME, "err:not_owner");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            if (r.members.size() < 2) {
                protocol::Message reply(GameCmd::START_GAME, "err:need_2");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            if (r.state != 0) {
                protocol::Message reply(GameCmd::START_GAME, "err:playing");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            r.state = 1;
            r.hp[0] = r.hp[1] = 100;
            r.turn = 0;
            r.round = 0;
            r.result.clear();
            protocol::Message reply(GameCmd::START_GAME, "ok");
            service::ProtocolRouterComponent::send_message(conn, reply);
            rooms->broadcast_game_state(it->second);
        });

    // GAME_ACTION
    router->register_handler(GameCmd::GAME_ACTION,
        [session, rooms](const net::TcpConnectionPtr& conn, const protocol::Message& msg) {
            if (!session->is_logged_in(conn)) {
                protocol::Message reply(GameCmd::GAME_ACTION, "err:login");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            auto it = rooms->conn_to_room_.find(conn.get());
            if (it == rooms->conn_to_room_.end()) {
                protocol::Message reply(GameCmd::GAME_ACTION, "err:no_room");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            Room& r = rooms->rooms_[it->second];
            if (r.state != 1) {
                protocol::Message reply(GameCmd::GAME_ACTION, "err:not_playing");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            int idx = -1;
            for (size_t i = 0; i < r.members.size(); ++i) {
                if (r.members[i].get() == conn.get()) { idx = static_cast<int>(i); break; }
            }
            if (idx < 0 || idx != r.turn) {
                protocol::Message reply(GameCmd::GAME_ACTION, "err:not_turn");
                service::ProtocolRouterComponent::send_message(conn, reply);
                return;
            }
            std::string action(msg.body.begin(), msg.body.end());
            while (!action.empty() && (action.back() == '\r' || action.back() == '\n')) action.pop_back();
            int other = 1 - idx;
            if (action == "attack") {
                r.hp[other] -= 10;
                if (r.hp[other] < 0) r.hp[other] = 0;
            }
            r.turn = other;
            r.round++;
            if (r.hp[0] <= 0 || r.hp[1] <= 0) {
                r.state = 0;
                r.result = r.hp[0] <= 0 ? "p2_win" : "p1_win";
            }
            protocol::Message reply(GameCmd::GAME_ACTION, "ok");
            service::ProtocolRouterComponent::send_message(conn, reply);
            rooms->broadcast_game_state(it->second);
        });

    svc.start();

    CHWELL_LOG_INFO("Game Server running on port " + std::to_string(cfg.listen_port()));

    static volatile sig_atomic_t g_stop = 0;
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGINT, [](int) { g_stop = 1; });

    if (isatty(STDIN_FILENO)) {
        std::cout << "Press ENTER to exit..." << std::endl;
        std::string line;
        std::getline(std::cin, line);
    } else {
        while (!g_stop) sleep(1);
    }

    svc.stop();
    return 0;
}
