/**
 * ChwellCore 新增模块使用示例
 * 
 * 展示以下模块的使用方法：
 * 1. TimerWheel - 时间轮定时器
 * 2. ConnectionPool - 连接池
 * 3. GridAoi - 九宫格 AOI
 * 4. EventBus - 事件总线
 * 5. RpcServer - RPC 服务端
 * 6. ObjectPool - 对象池
 * 7. TaskQueue - 异步任务队列
 * 8. RedisClient - Redis 客户端
 * 9. SlgMapManager - SLG 地图系统
 * 10. BattleSystem - 战斗系统
 */

#include <iostream>
#include <csignal>
#include <unistd.h>

#include "chwell/core/logger.h"
#include "chwell/core/timer_wheel.h"
#include "chwell/net/connection_pool.h"
#include "chwell/aoi/aoi.h"
#include "chwell/event/event_bus.h"
#include "chwell/rpc/rpc_server.h"
#include "chwell/pool/object_pool.h"
#include "chwell/task/task_queue.h"
#include "chwell/redis/redis_client.h"
#include "chwell/slg/map.h"
#include "chwell/slg/battle.h"

using namespace chwell;

volatile sig_atomic_t g_stop = 0;

void signal_handler(int) {
    g_stop = 1;
}

// Forward declarations
void demo_timer();
void demo_connection_pool();
void demo_aoi();
void demo_event_bus();
void demo_object_pool();
void demo_task_queue();
void demo_redis();
void demo_slg_map();
void demo_battle();

//=============================================================================
// 1. 定时器示例
//=============================================================================
void demo_timer() {
    std::cout << "\n=== Timer Demo ===\n";
    
    core::TimerWheel timer(100, 60, 4);
    timer.start();
    
    // 一次性定时器
    auto handle1 = timer.add_timer(1000, []() {
        std::cout << "[Timer] 1 second passed!\n";
    });
    
    // 重复定时器
    int count = 0;
    auto handle2 = timer.add_repeat_timer(500, [&count]() {
        std::cout << "[Timer] Repeat " << ++count << "\n";
        if (count >= 3) {
            std::cout << "[Timer] Stopping repeat timer\n";
        }
    });
    
    // 3秒后取消重复定时器
    timer.add_timer(2500, [&timer, &handle2]() {
        std::cout << "[Timer] Cancelling repeat timer\n";
        timer.cancel_timer(handle2);
    });
    
    sleep(4);
    timer.stop();
    std::cout << "[Timer] Demo done\n";
}

//=============================================================================
// 2. 连接池示例（简化演示）
//=============================================================================
void demo_connection_pool() {
    std::cout << "\n=== Connection Pool Demo ===\n";
    
    net::ConnectionPoolConfig config;
    config.host = "127.0.0.1";
    config.port = 9000;
    config.min_connections = 2;
    config.max_connections = 5;
    
    // 注意：实际使用需要有效的 IoService
    std::cout << "[Pool] Config: " << config.host << ":" << config.port << "\n";
    std::cout << "[Pool] Min: " << config.min_connections << ", Max: " << config.max_connections << "\n";
    std::cout << "[Pool] Demo done (skipped actual connection - no server)\n";
}

//=============================================================================
// 3. AOI 示例
//=============================================================================
void demo_aoi() {
    std::cout << "\n=== AOI Demo ===\n";
    
    aoi::GridAoi::Config config;
    config.map_width = 1000;
    config.map_height = 1000;
    config.grid_size = 100;
    config.view_range = 1;
    
    aoi::GridAoi aoi(config);
    
    // 设置回调
    aoi.set_callback([](const aoi::AoiEvent& event) {
        const char* type_str = "";
        switch (event.type) {
            case aoi::EventType::ENTER: type_str = "ENTER"; break;
            case aoi::EventType::LEAVE: type_str = "LEAVE"; break;
            case aoi::EventType::MOVE: type_str = "MOVE"; break;
        }
        std::cout << "[AOI] Event: " << type_str 
                  << ", watcher=" << event.watcher_id 
                  << ", target=" << event.target_id << "\n";
    });
    
    // 添加实体
    aoi::Entity e1(1, 150, 150, aoi::EntityType::PLAYER);
    aoi::Entity e2(2, 180, 180, aoi::EntityType::PLAYER);
    aoi::Entity e3(3, 500, 500, aoi::EntityType::PLAYER);
    
    aoi.add_entity(e1);
    aoi.add_entity(e2);
    aoi.add_entity(e3);
    
    std::cout << "[AOI] Total entities: " << aoi.total_entities() << "\n";
    
    // 获取视野内的实体
    auto in_view = aoi.get_entity_ids_in_view(150, 150);
    std::cout << "[AOI] Entities in view of (150,150): ";
    for (auto id : in_view) std::cout << id << " ";
    std::cout << "\n";
    
    // 移动实体
    aoi.update_entity(2, 50, 50);
    
    in_view = aoi.get_entity_ids_in_view(150, 150);
    std::cout << "[AOI] After move, entities in view of (150,150): ";
    for (auto id : in_view) std::cout << id << " ";
    std::cout << "\n";
    
    std::cout << "[AOI] Demo done\n";
}

//=============================================================================
// 4. 事件总线示例
//=============================================================================
void demo_event_bus() {
    std::cout << "\n=== Event Bus Demo ===\n";
    
    auto& bus = event::EventBus::instance();
    
    // 订阅事件
    auto handler1 = bus.subscribe<event::ConnectionEvent>([](const event::ConnectionEvent& e) {
        const char* type = e.type() == event::ConnectionEvent::Type::CONNECT ? "CONNECT" : "DISCONNECT";
        std::cout << "[Event] Connection: " << type << ", id=" << e.connection_id() << "\n";
    });
    
    auto handler2 = bus.subscribe<event::ChatEvent>([](const event::ChatEvent& e) {
        std::cout << "[Event] Chat from " << e.sender() 
                  << " in " << e.channel() << ": " << e.message() << "\n";
    });
    
    // 发布事件
    event::ConnectionEvent conn_event(event::ConnectionEvent::Type::CONNECT, 12345, "127.0.0.1");
    bus.publish(conn_event);
    
    event::ChatEvent chat_event("player1", "world", "Hello World!");
    bus.publish(chat_event);
    
    // 取消订阅
    bus.unsubscribe(handler1);
    bus.unsubscribe(handler2);
    
    std::cout << "[Event] Demo done\n";
}

//=============================================================================
// 5. 对象池示例
//=============================================================================
void demo_object_pool() {
    std::cout << "\n=== Object Pool Demo ===\n";
    
    // 字符串对象池
    struct StringPoolConfig {
        int initial_size = 5;
        int max_size = 20;
    };
    
    pool::ObjectPoolConfig<std::string> config;
    config.initial_size = 5;
    config.max_size = 10;
    
    pool::ObjectFactory<std::string> factory;
    factory.create = []() { return std::make_unique<std::string>(); };
    factory.reset = [](std::string* s) { s->clear(); };
    
    pool::ObjectPool<std::string> pool(config, factory);
    
    std::cout << "[Pool] Initial: created=" << pool.created_count() 
              << ", available=" << pool.available_count() << "\n";
    
    // 获取对象
    auto obj1 = pool.acquire();
    *obj1 = "Hello";
    
    auto obj2 = pool.acquire();
    *obj2 = "World";
    
    std::cout << "[Pool] After borrow: created=" << pool.created_count() 
              << ", borrowed=" << pool.borrowed_count() 
              << ", available=" << pool.available_count() << "\n";
    
    std::cout << "[Pool] Objects: " << *obj1 << ", " << *obj2 << "\n";
    
    // 归还（自动）
    obj1.reset();
    obj2.reset();
    
    std::cout << "[Pool] After return: available=" << pool.available_count() << "\n";
    
    // 缓冲区池
    auto buf_pool = std::make_shared<pool::BufferPool>(1024, 3, 10);
    auto buf = buf_pool->acquire();
    buf->insert(buf->end(), {'t', 'e', 's', 't'});
    std::cout << "[BufferPool] Buffer size: " << buf->size() << ", capacity: " << buf->capacity() << "\n";
    
    std::cout << "[Pool] Demo done\n";
}

//=============================================================================
// 6. 任务队列示例
//=============================================================================
void demo_task_queue() {
    std::cout << "\n=== Task Queue Demo ===\n";
    
    task::TaskQueue::Config config;
    config.worker_threads = 2;
    config.max_queue_size = 100;
    
    task::TaskQueue queue(config);
    queue.start();
    
    std::atomic<int> counter{0};
    
    // 提交多个任务
    for (int i = 0; i < 5; ++i) {
        queue.submit<int>(
            [&counter, i]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                int val = counter.fetch_add(1);
                std::cout << "[Task] Task " << i << " executed, counter=" << val + 1 << "\n";
                return val + 1;
            },
            [](const task::TaskResult<int>& result) {
                if (result.ok()) {
                    std::cout << "[Task] Callback: result=" << result.value << "\n";
                } else {
                    std::cout << "[Task] Callback: failed - " << result.error << "\n";
                }
            },
            task::TaskPriority::NORMAL
        );
    }
    
    // 等待完成
    queue.wait_all();
    
    std::cout << "[Task] Completed: " << queue.completed_count() << "\n";
    queue.stop();
    
    std::cout << "[Task] Demo done\n";
}

//=============================================================================
// Main
//=============================================================================
int main() {
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);
    
    std::cout << "========================================\n";
    std::cout << "  ChwellCore New Modules Demo\n";
    std::cout << "========================================\n";
    
    // 运行各个示例
    demo_timer();
    demo_connection_pool();
    demo_aoi();
    demo_event_bus();
    demo_object_pool();
    demo_task_queue();
    demo_redis();
    demo_slg_map();
    demo_battle();
    
    std::cout << "\n========================================\n";
    std::cout << "  All demos completed!\n";
    std::cout << "========================================\n";
    
    return 0;
}

//=============================================================================
// 8. Redis 客户端示例
//=============================================================================
void demo_redis() {
    std::cout << "\n=== Redis Demo ===\n";
    
    redis::RedisClient client;
    client.connect();
    
    // 字符串操作
    client.set("player:1:name", "Alice");
    client.setex("session:abc", 3600, "user_data");
    
    std::string name;
    if (client.get("player:1:name", name)) {
        std::cout << "[Redis] player:1:name = " << name << "\n";
    }
    
    // 自增
    client.set("counter", "0");
    int64_t val = client.incr("counter");
    std::cout << "[Redis] counter after incr = " << val << "\n";
    
    // Hash 操作
    client.hset("player:1:info", "level", "10");
    client.hset("player:1:info", "exp", "5000");
    
    auto info = client.hgetall("player:1:info");
    std::cout << "[Redis] player:1:info = { ";
    for (const auto& p : info) {
        std::cout << p.first << ":" << p.second << " ";
    }
    std::cout << "}\n";
    
    // List 操作
    client.lpush("messages", "Hello");
    client.lpush("messages", "World");
    auto messages = client.lrange("messages", 0, -1);
    std::cout << "[Redis] messages = [ ";
    for (const auto& m : messages) std::cout << m << " ";
    std::cout << "]\n";
    
    std::cout << "[Redis] Demo done\n";
}

//=============================================================================
// 9. SLG 地图示例
//=============================================================================
void demo_slg_map() {
    std::cout << "\n=== SLG Map Demo ===\n";
    
    slg::SlgMapConfig config;
    config.width = 100;
    config.height = 100;
    
    slg::SlgMapManager map(config);
    map.init();
    map.generate_terrain();
    map.generate_resources(50);
    map.generate_cities(5);
    
    std::cout << "[Map] Created " << config.width << "x" << config.height << " map\n";
    std::cout << "[Map] Cities: " << map.total_cities() << "\n";
    
    // 获取格子信息
    slg::GridCell cell;
    if (map.get_cell(50, 50, cell)) {
        std::cout << "[Map] Cell(50,50) terrain = " << (int)cell.terrain << "\n";
    }
    
    // 创建部队
    uint64_t troop_id = map.create_troop(1, 10, 10, 100, 50, 30, 10);
    std::cout << "[Map] Created troop " << troop_id << "\n";
    
    // 移动部队
    map.move_troop(troop_id, 20, 20);
    std::cout << "[Map] Troop moving to (20,20)\n";
    
    std::cout << "[Map] Demo done\n";
}

//=============================================================================
// 10. 战斗系统示例
//=============================================================================
void demo_battle() {
    std::cout << "\n=== Battle Demo ===\n";
    
    slg::BattleSystem battle;
    
    // 创建武将
    std::vector<slg::General> attacker_generals = {
        []() { slg::General g; g.general_id = 1; g.name = "关羽"; g.force = 97; g.intelligence = 75; g.command = 88; g.speed = 70; return g; }(),
        []() { slg::General g; g.general_id = 2; g.name = "张飞"; g.force = 98; g.intelligence = 30; g.command = 75; g.speed = 65; return g; }()
    };
    
    std::vector<slg::General> defender_generals = {
        []() { slg::General g; g.general_id = 3; g.name = "吕布"; g.force = 100; g.intelligence = 32; g.command = 68; g.speed = 90; return g; }()
    };
    
    // 执行战斗
    auto report = battle.execute(
        1, "刘备军", attacker_generals, 5000,
        2, "吕布军", defender_generals, 3000,
        false
    );
    
    std::cout << "[Battle] Attacker: " << report.attacker_name 
              << " (" << report.attacker_initial << " -> " << report.attacker_final << ")\n";
    std::cout << "[Battle] Defender: " << report.defender_name 
              << " (" << report.defender_initial << " -> " << report.defender_final << ")\n";
    
    const char* result_str = "";
    switch (report.result) {
        case slg::BattleReport::Result::ATTACKER_WIN: result_str = "ATTACKER_WIN"; break;
        case slg::BattleReport::Result::DEFENDER_WIN: result_str = "DEFENDER_WIN"; break;
        case slg::BattleReport::Result::DRAW: result_str = "DRAW"; break;
    }
    std::cout << "[Battle] Result: " << result_str << "\n";
    
    std::cout << "[Battle] Rounds: " << report.rounds.size() << "\n";
    if (!report.rounds.empty()) {
        std::cout << "[Battle] First round actions: " 
                  << report.rounds[0].attacker_actions.size() + report.rounds[0].defender_actions.size() 
                  << "\n";
    }
    
    std::cout << "[Battle] Demo done\n";
}