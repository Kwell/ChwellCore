#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>

#include "chwell/aoi/aoi.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace slg {

// 地形类型
enum class TerrainType {
    PLAIN,      // 平原
    FOREST,     // 森林
    MOUNTAIN,   // 山地
    WATER,      // 水域
    CITY,       // 城池
    RESOURCE,   // 资源点
    CAMP        // 要塞
};

// 格子数据
struct GridCell {
    int x, y;
    TerrainType terrain;
    
    // 所属玩家/联盟
    uint64_t owner_id;
    uint64_t alliance_id;
    
    // 建筑数据
    int building_type;      // 0=无建筑
    int building_level;
    int64_t building_complete_time;
    
    // 资源数据
    int resource_type;      // 0=无, 1=粮食, 2=木材, 3=石料, 4=铁矿
    int64_t resource_amount;
    int resource_gather_rate;
    
    // 驻扎部队
    std::vector<uint64_t> troops;
    
    GridCell()
        : x(0), y(0), terrain(TerrainType::PLAIN)
        , owner_id(0), alliance_id(0)
        , building_type(0), building_level(0), building_complete_time(0)
        , resource_type(0), resource_amount(0), resource_gather_rate(0) {}
    
    bool has_building() const { return building_type > 0; }
    bool has_resource() const { return resource_type > 0; }
    bool has_owner() const { return owner_id > 0; }
    bool has_troops() const { return !troops.empty(); }
};

// 城池数据
struct City {
    uint64_t city_id;
    uint64_t owner_id;
    uint64_t alliance_id;
    
    int x, y;
    std::string name;
    int level;
    int durability;         // 耐久度
    int max_durability;
    
    // 产出
    int food_production;
    int wood_production;
    int stone_production;
    int iron_production;
    
    // 范围
    int territory_range;    // 领地范围
    
    City() : city_id(0), owner_id(0), alliance_id(0)
           , x(0), y(0), level(1), durability(100), max_durability(100)
           , food_production(100), wood_production(50), stone_production(30), iron_production(20)
           , territory_range(3) {}
};

// 部队数据
struct Troop {
    uint64_t troop_id;
    uint64_t owner_id;
    
    int from_x, from_y;
    int to_x, to_y;
    int current_x, current_y;
    
    enum class State {
        IDLE,       // 空闲
        MARCHING,   // 行军中
        GATHERING,  // 采集中
        BATTLE,     // 战斗中
        RETURNING   // 返回中
    } state;
    
    int64_t start_time;
    int64_t arrive_time;
    
    // 兵力
    int infantry;       // 步兵
    int cavalry;        // 骑兵
    int archer;         // 弓兵
    int siege;          // 攻城
    
    // 携带资源
    int64_t carry_food;
    int64_t carry_wood;
    int64_t carry_stone;
    int64_t carry_iron;
    
    Troop() : troop_id(0), owner_id(0)
            , from_x(0), from_y(0), to_x(0), to_y(0), current_x(0), current_y(0)
            , state(State::IDLE), start_time(0), arrive_time(0)
            , infantry(0), cavalry(0), archer(0), siege(0)
            , carry_food(0), carry_wood(0), carry_stone(0), carry_iron(0) {}
    
    int total_soldiers() const {
        return infantry + cavalry + archer + siege;
    }
    
    int64_t total_carry() const {
        return carry_food + carry_wood + carry_stone + carry_iron;
    }
    
    int march_speed() const {
        // 基础速度 + 骑兵加成
        int base = 100;
        if (cavalry > 0) {
            base += 20 * cavalry / (total_soldiers() + 1);
        }
        return base;
    }
};

// SLG 地图配置
struct SlgMapConfig {
    int width;
    int height;
    int cell_size;          // 每格游戏单位
    int view_range;         // 视野范围
    
    SlgMapConfig()
        : width(500), height(500), cell_size(1), view_range(5) {}
};

// SLG 地图管理器
class SlgMapManager {
public:
    using CellCallback = std::function<void(const GridCell&)>;
    using TroopCallback = std::function<void(const Troop&)>;
    
    explicit SlgMapManager(const SlgMapConfig& config = SlgMapConfig());
    
    // 初始化地图
    void init();
    
    // 生成地形（简化版）
    void generate_terrain();
    
    // 生成资源点
    void generate_resources(int count);
    
    // 生成城池
    void generate_cities(int count);
    
    // 格子操作
    bool get_cell(int x, int y, GridCell& cell) const;
    bool update_cell(int x, int y, const GridCell& cell);
    bool update_cell_owner(int x, int y, uint64_t owner_id, uint64_t alliance_id = 0);
    bool add_building(int x, int y, int type, int level);
    bool remove_building(int x, int y);
    
    // 城池操作
    bool create_city(int x, int y, const std::string& name, uint64_t owner_id);
    bool get_city(uint64_t city_id, City& city) const;
    bool get_city_at(int x, int y, City& city) const;
    bool update_city(const City& city);
    
    // 部队操作
    uint64_t create_troop(uint64_t owner_id, int from_x, int from_y, 
                          int infantry, int cavalry, int archer, int siege);
    bool move_troop(uint64_t troop_id, int to_x, int to_y);
    bool cancel_troop(uint64_t troop_id);
    bool get_troop(uint64_t troop_id, Troop& troop) const;
    
    // 更新部队位置（时间推进）
    void update_troops(int64_t current_time);
    
    // 查询
    std::vector<GridCell> get_cells_in_range(int x, int y, int range) const;
    std::vector<Troop> get_troops_in_range(int x, int y, int range) const;
    std::vector<City> get_cities_in_range(int x, int y, int range) const;
    std::vector<GridCell> get_player_territory(uint64_t player_id) const;
    
    // 路径计算（A*简化版）
    std::vector<std::pair<int, int>> find_path(int from_x, int from_y, int to_x, int to_y) const;
    int calculate_distance(int x1, int y1, int x2, int y2) const;
    int64_t calculate_march_time(int from_x, int from_y, int to_x, int to_y, int speed) const;
    
    // 回调
    void set_cell_callback(CellCallback cb) { cell_callback_ = std::move(cb); }
    void set_troop_callback(TroopCallback cb) { troop_callback_ = std::move(cb); }
    
    // AOI 集成
    aoi::GridAoi& aoi() { return aoi_; }
    const aoi::GridAoi& aoi() const { return aoi_; }
    
    // 统计
    int total_cells() const { return config_.width * config_.height; }
    int total_cities() const;
    int total_troops() const;
    
    const SlgMapConfig& config() const { return config_; }
    
private:
    int cell_index(int x, int y) const;
    bool is_valid_pos(int x, int y) const;
    void notify_cell_change(const GridCell& cell);
    void notify_troop_change(const Troop& troop);
    
    SlgMapConfig config_;
    mutable std::mutex mutex_;
    
    std::vector<GridCell> cells_;
    std::unordered_map<uint64_t, City> cities_;
    std::unordered_map<uint64_t, Troop> troops_;
    
    uint64_t next_city_id_;
    uint64_t next_troop_id_;
    
    aoi::GridAoi aoi_;
    
    CellCallback cell_callback_;
    TroopCallback troop_callback_;
};

} // namespace slg
} // namespace chwell