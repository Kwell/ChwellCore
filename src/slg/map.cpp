#include "chwell/slg/map.h"
#include <algorithm>
#include <queue>
#include <cmath>

namespace chwell {
namespace slg {

SlgMapManager::SlgMapManager(const SlgMapConfig& config)
    : config_(config)
    , next_city_id_(1)
    , next_troop_id_(1)
    , aoi_(aoi::GridAoi::Config()) {
}

void SlgMapManager::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 初始化所有格子
    cells_.resize(config_.width * config_.height);
    for (int y = 0; y < config_.height; ++y) {
        for (int x = 0; x < config_.width; ++x) {
            auto& cell = cells_[cell_index(x, y)];
            cell.x = x;
            cell.y = y;
            cell.terrain = TerrainType::PLAIN;
        }
    }
    
    CHWELL_LOG_INFO("SlgMap initialized: " << config_.width << "x" << config_.height);
}

void SlgMapManager::generate_terrain() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 简化地形生成：随机分布
    for (auto& cell : cells_) {
        int r = rand() % 100;
        if (r < 60) {
            cell.terrain = TerrainType::PLAIN;
        } else if (r < 75) {
            cell.terrain = TerrainType::FOREST;
        } else if (r < 85) {
            cell.terrain = TerrainType::MOUNTAIN;
        } else if (r < 95) {
            cell.terrain = TerrainType::WATER;
        } else {
            cell.terrain = TerrainType::PLAIN;
        }
    }
    
    CHWELL_LOG_INFO("Terrain generated");
}

void SlgMapManager::generate_resources(int count) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (int i = 0; i < count; ++i) {
        int x = rand() % config_.width;
        int y = rand() % config_.height;
        auto& cell = cells_[cell_index(x, y)];
        
        // 只在平原生成资源
        if (cell.terrain == TerrainType::PLAIN && !cell.has_resource() && !cell.has_building()) {
            cell.terrain = TerrainType::RESOURCE;
            cell.resource_type = (rand() % 4) + 1;  // 1-4
            cell.resource_amount = 10000 + (rand() % 90000);
            cell.resource_gather_rate = 100 + (rand() % 200);
        }
    }
    
    CHWELL_LOG_INFO("Resources generated: " << count);
}

void SlgMapManager::generate_cities(int count) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 边界保护：确保地图足够大
    int margin = std::min(50, std::min(config_.width, config_.height) / 4);
    int x_range = config_.width - 2 * margin;
    int y_range = config_.height - 2 * margin;
    if (x_range <= 0 || y_range <= 0) {
        CHWELL_LOG_WARN("Map too small for city generation");
        return;
    }
    
    int created = 0;
    int attempts = 0;
    int max_attempts = count * 100;
    
    while (created < count && attempts < max_attempts) {
        int x = margin + rand() % x_range;
        int y = margin + rand() % y_range;
        
        auto& cell = cells_[cell_index(x, y)];
        
        // 检查周围是否有城池
        bool too_close = false;
        for (const auto& pair : cities_) {
            int dx = std::abs(pair.second.x - x);
            int dy = std::abs(pair.second.y - y);
            if (dx < 20 && dy < 20) {
                too_close = true;
                break;
            }
        }
        
        if (!too_close && cell.terrain == TerrainType::PLAIN) {
            City city;
            city.city_id = next_city_id_++;
            city.x = x;
            city.y = y;
            city.name = "City_" + std::to_string(city.city_id);
            city.level = 1 + rand() % 5;
            
            cell.terrain = TerrainType::CITY;
            cell.owner_id = city.city_id;
            
            cities_[city.city_id] = city;
            ++created;
        }
        
        ++attempts;
    }
    
    CHWELL_LOG_INFO("Cities generated: " << created);
}

int SlgMapManager::cell_index(int x, int y) const {
    return y * config_.width + x;
}

bool SlgMapManager::is_valid_pos(int x, int y) const {
    return x >= 0 && x < config_.width && y >= 0 && y < config_.height;
}

bool SlgMapManager::get_cell(int x, int y, GridCell& cell) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_valid_pos(x, y)) return false;
    cell = cells_[cell_index(x, y)];
    return true;
}

bool SlgMapManager::update_cell(int x, int y, const GridCell& cell) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_valid_pos(x, y)) return false;
    cells_[cell_index(x, y)] = cell;
    notify_cell_change(cell);
    return true;
}

bool SlgMapManager::update_cell_owner(int x, int y, uint64_t owner_id, uint64_t alliance_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_valid_pos(x, y)) return false;
    auto& cell = cells_[cell_index(x, y)];
    cell.owner_id = owner_id;
    cell.alliance_id = alliance_id;
    notify_cell_change(cell);
    return true;
}

bool SlgMapManager::add_building(int x, int y, int type, int level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_valid_pos(x, y)) return false;
    auto& cell = cells_[cell_index(x, y)];
    if (cell.has_building()) return false;
    cell.building_type = type;
    cell.building_level = level;
    notify_cell_change(cell);
    return true;
}

bool SlgMapManager::remove_building(int x, int y) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_valid_pos(x, y)) return false;
    auto& cell = cells_[cell_index(x, y)];
    cell.building_type = 0;
    cell.building_level = 0;
    notify_cell_change(cell);
    return true;
}

bool SlgMapManager::create_city(int x, int y, const std::string& name, uint64_t owner_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_valid_pos(x, y)) return false;
    
    auto& cell = cells_[cell_index(x, y)];
    if (cell.terrain != TerrainType::PLAIN) return false;
    
    City city;
    city.city_id = next_city_id_++;
    city.x = x;
    city.y = y;
    city.name = name;
    city.owner_id = owner_id;
    city.level = 1;
    
    cell.terrain = TerrainType::CITY;
    cell.owner_id = owner_id;
    
    cities_[city.city_id] = city;
    notify_cell_change(cell);
    
    return true;
}

bool SlgMapManager::get_city(uint64_t city_id, City& city) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cities_.find(city_id);
    if (it != cities_.end()) {
        city = it->second;
        return true;
    }
    return false;
}

bool SlgMapManager::get_city_at(int x, int y, City& city) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& pair : cities_) {
        if (pair.second.x == x && pair.second.y == y) {
            city = pair.second;
            return true;
        }
    }
    return false;
}

bool SlgMapManager::update_city(const City& city) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cities_.find(city.city_id);
    if (it == cities_.end()) return false;
    it->second = city;
    return true;
}

uint64_t SlgMapManager::create_troop(uint64_t owner_id, int from_x, int from_y,
                                     int infantry, int cavalry, int archer, int siege) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Troop troop;
    troop.troop_id = next_troop_id_++;
    troop.owner_id = owner_id;
    troop.from_x = from_x;
    troop.from_y = from_y;
    troop.current_x = from_x;
    troop.current_y = from_y;
    troop.infantry = infantry;
    troop.cavalry = cavalry;
    troop.archer = archer;
    troop.siege = siege;
    troop.state = Troop::State::IDLE;
    
    troops_[troop.troop_id] = troop;
    
    // 添加到 AOI
    aoi_.add_entity(aoi::Entity(troop.troop_id, from_x, from_y, aoi::EntityType::OTHER));
    
    notify_troop_change(troop);
    
    return troop.troop_id;
}

bool SlgMapManager::move_troop(uint64_t troop_id, int to_x, int to_y) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = troops_.find(troop_id);
    if (it == troops_.end()) return false;
    
    auto& troop = it->second;
    if (troop.state != Troop::State::IDLE) return false;
    
    troop.to_x = to_x;
    troop.to_y = to_y;
    troop.state = Troop::State::MARCHING;
    troop.start_time = std::chrono::system_clock::now().time_since_epoch().count() / 1000000;
    
    int distance = calculate_distance(troop.from_x, troop.from_y, to_x, to_y);
    troop.arrive_time = troop.start_time + calculate_march_time(
        troop.from_x, troop.from_y, to_x, to_y, troop.march_speed());
    
    notify_troop_change(troop);
    
    return true;
}

bool SlgMapManager::cancel_troop(uint64_t troop_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = troops_.find(troop_id);
    if (it == troops_.end()) return false;
    
    auto& troop = it->second;
    if (troop.state != Troop::State::MARCHING) return false;
    
    // 返回原点
    troop.to_x = troop.from_x;
    troop.to_y = troop.from_y;
    troop.state = Troop::State::RETURNING;
    
    notify_troop_change(troop);
    
    return true;
}

bool SlgMapManager::get_troop(uint64_t troop_id, Troop& troop) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = troops_.find(troop_id);
    if (it != troops_.end()) {
        troop = it->second;
        return true;
    }
    return false;
}

void SlgMapManager::update_troops(int64_t current_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& pair : troops_) {
        auto& troop = pair.second;
        
        if (troop.state == Troop::State::MARCHING || 
            troop.state == Troop::State::RETURNING) {
            
            if (current_time >= troop.arrive_time) {
                // 到达目的地
                troop.current_x = troop.to_x;
                troop.current_y = troop.to_y;
                
                if (troop.state == Troop::State::RETURNING) {
                    troop.state = Troop::State::IDLE;
                    troop.from_x = troop.to_x;
                    troop.from_y = troop.to_y;
                } else {
                    // 检查目的地状态，决定下一步
                    auto& cell = cells_[cell_index(troop.to_x, troop.to_y)];
                    if (cell.has_resource()) {
                        troop.state = Troop::State::GATHERING;
                    } else if (cell.has_owner() && cell.owner_id != troop.owner_id) {
                        troop.state = Troop::State::BATTLE;
                    } else {
                        troop.state = Troop::State::IDLE;
                        troop.from_x = troop.to_x;
                        troop.from_y = troop.to_y;
                    }
                }
                
                // 更新 AOI
                aoi_.update_entity(troop.troop_id, troop.current_x, troop.current_y);
                
                notify_troop_change(troop);
            } else {
                // 更新当前位置（插值）
                int64_t elapsed = current_time - troop.start_time;
                int64_t total = troop.arrive_time - troop.start_time;
                if (total > 0) {
                    double progress = static_cast<double>(elapsed) / total;
                    troop.current_x = troop.from_x + static_cast<int>((troop.to_x - troop.from_x) * progress);
                    troop.current_y = troop.from_y + static_cast<int>((troop.to_y - troop.from_y) * progress);
                }
            }
        }
    }
}

std::vector<GridCell> SlgMapManager::get_cells_in_range(int x, int y, int range) const {
    std::vector<GridCell> result;
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (int dy = -range; dy <= range; ++dy) {
        for (int dx = -range; dx <= range; ++dx) {
            int nx = x + dx, ny = y + dy;
            if (is_valid_pos(nx, ny)) {
                result.push_back(cells_[cell_index(nx, ny)]);
            }
        }
    }
    
    return result;
}

std::vector<Troop> SlgMapManager::get_troops_in_range(int x, int y, int range) const {
    std::vector<Troop> result;
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (const auto& pair : troops_) {
        const auto& troop = pair.second;
        if (calculate_distance(x, y, troop.current_x, troop.current_y) <= range) {
            result.push_back(troop);
        }
    }
    
    return result;
}

std::vector<City> SlgMapManager::get_cities_in_range(int x, int y, int range) const {
    std::vector<City> result;
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (const auto& pair : cities_) {
        const auto& city = pair.second;
        if (calculate_distance(x, y, city.x, city.y) <= range) {
            result.push_back(city);
        }
    }
    
    return result;
}

std::vector<GridCell> SlgMapManager::get_player_territory(uint64_t player_id) const {
    std::vector<GridCell> result;
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (const auto& cell : cells_) {
        if (cell.owner_id == player_id) {
            result.push_back(cell);
        }
    }
    
    return result;
}

int SlgMapManager::calculate_distance(int x1, int y1, int x2, int y2) const {
    return std::max(std::abs(x2 - x1), std::abs(y2 - y1));  // 曼哈顿距离变体
}

int64_t SlgMapManager::calculate_march_time(int from_x, int from_y, int to_x, int to_y, int speed) const {
    int distance = calculate_distance(from_x, from_y, to_x, to_y);
    // 基础时间 = 距离 * 1000ms / 速度
    return static_cast<int64_t>(distance) * 1000 / speed;
}

std::vector<std::pair<int, int>> SlgMapManager::find_path(int from_x, int from_y, int to_x, int to_y) const {
    // 简化的 A* 实现
    std::vector<std::pair<int, int>> path;
    
    if (!is_valid_pos(from_x, from_y) || !is_valid_pos(to_x, to_y)) {
        return path;
    }
    
    // 简化：直线移动
    int dx = to_x > from_x ? 1 : (to_x < from_x ? -1 : 0);
    int dy = to_y > from_y ? 1 : (to_y < from_y ? -1 : 0);
    
    int x = from_x, y = from_y;
    while (x != to_x || y != to_y) {
        path.push_back({x, y});
        if (x != to_x) x += dx;
        else if (y != to_y) y += dy;
    }
    path.push_back({to_x, to_y});
    
    return path;
}

int SlgMapManager::total_cities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(cities_.size());
}

int SlgMapManager::total_troops() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(troops_.size());
}

void SlgMapManager::notify_cell_change(const GridCell& cell) {
    if (cell_callback_) {
        cell_callback_(cell);
    }
}

void SlgMapManager::notify_troop_change(const Troop& troop) {
    if (troop_callback_) {
        troop_callback_(troop);
    }
}

} // namespace slg
} // namespace chwell