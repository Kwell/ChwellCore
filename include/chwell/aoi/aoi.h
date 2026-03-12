#pragma once

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>
#include <mutex>
#include <cmath>

#include "chwell/core/logger.h"

namespace chwell {
namespace aoi {

// AOI 实体类型
enum class EntityType {
    PLAYER,     // 玩家
    NPC,        // NPC
    MONSTER,    // 怪物
    ITEM,       // 物品
    BUILDING,   // 建筑
    OTHER       // 其他
};

// AOI 实体
struct Entity {
    uint64_t id;            // 实体ID
    int x;                  // X坐标
    int y;                  // Y坐标
    EntityType type;        // 实体类型
    void* user_data;        // 用户数据
    
    Entity() : id(0), x(0), y(0), type(EntityType::OTHER), user_data(nullptr) {}
    Entity(uint64_t id, int x, int y, EntityType type = EntityType::OTHER, void* data = nullptr)
        : id(id), x(x), y(y), type(type), user_data(data) {}
};

// AOI 事件类型
enum class EventType {
    ENTER,      // 进入视野
    LEAVE,      // 离开视野
    MOVE        // 移动
};

// AOI 事件
struct AoiEvent {
    EventType type;
    uint64_t watcher_id;    // 观察者ID
    uint64_t target_id;     // 目标实体ID
    int old_x, old_y;       // 旧坐标（仅MOVE事件）
    int new_x, new_y;       // 新坐标
    
    AoiEvent() : type(EventType::ENTER), watcher_id(0), target_id(0),
                 old_x(0), old_y(0), new_x(0), new_y(0) {}
};

// AOI 回调类型
using AoiCallback = std::function<void(const AoiEvent&)>;

// 九宫格 AOI 管理器
// 适用于大地图、低更新频率的场景（如 SLG）
class GridAoi {
public:
    // 配置
    struct Config {
        int map_width;          // 地图宽度
        int map_height;         // 地图高度
        int grid_size;          // 格子大小
        int view_range;         // 视野范围（格子数）
        
        Config() : map_width(1000), map_height(1000), 
                   grid_size(50), view_range(1) {}
        
        int grid_x_count() const { return (map_width + grid_size - 1) / grid_size; }
        int grid_y_count() const { return (map_height + grid_size - 1) / grid_size; }
    };
    
    explicit GridAoi(const Config& config = Config());
    ~GridAoi() = default;
    
    // 添加实体
    bool add_entity(const Entity& entity);
    
    // 移除实体
    bool remove_entity(uint64_t entity_id);
    
    // 更新实体位置
    bool update_entity(uint64_t entity_id, int new_x, int new_y);
    
    // 获取实体
    bool get_entity(uint64_t entity_id, Entity& entity) const;
    
    // 获取视野内的实体
    std::vector<Entity> get_entities_in_view(uint64_t watcher_id) const;
    std::vector<Entity> get_entities_in_view(int x, int y) const;
    
    // 获取视野内的实体ID
    std::vector<uint64_t> get_entity_ids_in_view(uint64_t watcher_id) const;
    std::vector<uint64_t> get_entity_ids_in_view(int x, int y) const;
    
    // 设置回调
    void set_callback(AoiCallback callback) { callback_ = std::move(callback); }
    
    // 获取格子内的实体
    std::vector<Entity> get_entities_in_grid(int grid_x, int grid_y) const;
    
    // 获取格子内的实体ID
    std::vector<uint64_t> get_entity_ids_in_grid(int grid_x, int grid_y) const;
    
    // 获取九宫格内的实体（用于调试）
    std::vector<Entity> get_entities_in_grids(int center_grid_x, int center_grid_y) const;
    
    // 坐标转格子
    void pos_to_grid(int x, int y, int& grid_x, int& grid_y) const;
    
    // 格子转坐标（格子中心）
    void grid_to_center(int grid_x, int grid_y, int& x, int& y) const;
    
    // 获取配置
    const Config& config() const { return config_; }
    
    // 获取统计
    int total_entities() const;
    int entity_count_in_grid(int grid_x, int grid_y) const;
    
private:
    // 格子索引
    int grid_index(int grid_x, int grid_y) const;
    
    // 检查格子是否有效
    bool is_valid_grid(int grid_x, int grid_y) const;
    
    // 获取九宫格范围
    void get_grids_in_view(int x, int y, 
                           int& min_gx, int& min_gy, 
                           int& max_gx, int& max_gy) const;
    
    // 触发事件
    void trigger_event(const AoiEvent& event);
    
    Config config_;
    mutable std::mutex mutex_;
    
    // 实体存储
    std::unordered_map<uint64_t, Entity> entities_;
    
    // 格子存储 [grid_index] -> set of entity_ids
    std::vector<std::unordered_set<uint64_t>> grids_;
    
    // 实体所在格子缓存
    std::unordered_map<uint64_t, int> entity_to_grid_;
    
    // 回调
    AoiCallback callback_;
};

// 十字链表 AOI（适用于实时战斗、高更新频率）
// 注：这是一个简化的实现，完整实现需要更复杂的链表操作
class CrossListAoi {
public:
    struct Config {
        int map_width;
        int map_height;
        int view_range;  // 视野半径
        
        Config() : map_width(1000), map_height(1000), view_range(100) {}
    };
    
    explicit CrossListAoi(const Config& config = Config());
    ~CrossListAoi() = default;
    
    bool add_entity(const Entity& entity);
    bool remove_entity(uint64_t entity_id);
    bool update_entity(uint64_t entity_id, int new_x, int new_y);
    
    std::vector<Entity> get_entities_in_view(uint64_t watcher_id) const;
    std::vector<Entity> get_entities_in_view(int x, int y) const;
    
    void set_callback(AoiCallback callback) { callback_ = std::move(callback); }
    
    const Config& config() const { return config_; }
    int total_entities() const;
    
private:
    struct Node {
        Entity entity;
        Node* prev_x;
        Node* next_x;
        Node* prev_y;
        Node* next_y;
        
        Node() : prev_x(nullptr), next_x(nullptr), 
                 prev_y(nullptr), next_y(nullptr) {}
    };
    
    void insert_to_x_list(Node* node);
    void insert_to_y_list(Node* node);
    void remove_from_lists(Node* node);
    
    Config config_;
    mutable std::mutex mutex_;
    
    std::unordered_map<uint64_t, std::unique_ptr<Node>> nodes_;
    Node* head_x_;  // X链表头
    Node* head_y_;  // Y链表头
    
    AoiCallback callback_;
};

// AOI 管理器（工厂模式）
class AoiManager {
public:
    enum class AoiType {
        GRID,       // 九宫格
        CROSS_LIST  // 十字链表
    };
    
    static std::unique_ptr<GridAoi> create_grid_aoi(const GridAoi::Config& config = GridAoi::Config()) {
        return std::make_unique<GridAoi>(config);
    }
    
    static std::unique_ptr<CrossListAoi> create_cross_list_aoi(const CrossListAoi::Config& config = CrossListAoi::Config()) {
        return std::make_unique<CrossListAoi>(config);
    }
};

} // namespace aoi
} // namespace chwell