#include "chwell/aoi/aoi.h"
#include <algorithm>

namespace chwell {
namespace aoi {

//=============================================================================
// GridAoi 实现
//=============================================================================

GridAoi::GridAoi(const Config& config)
    : config_(config) {
    // 初始化格子
    int grid_count = config_.grid_x_count() * config_.grid_y_count();
    grids_.resize(grid_count);
    
    CHWELL_LOG_INFO("GridAoi initialized: map=" << config_.map_width << "x" << config_.map_height
                  << ", grid_size=" << config_.grid_size
                  << ", grids=" << config_.grid_x_count() << "x" << config_.grid_y_count()
                  << ", view_range=" << config_.view_range);
}

int GridAoi::grid_index(int grid_x, int grid_y) const {
    return grid_y * config_.grid_x_count() + grid_x;
}

bool GridAoi::is_valid_grid(int grid_x, int grid_y) const {
    return grid_x >= 0 && grid_x < config_.grid_x_count() &&
           grid_y >= 0 && grid_y < config_.grid_y_count();
}

void GridAoi::pos_to_grid(int x, int y, int& grid_x, int& grid_y) const {
    grid_x = std::max(0, std::min(x / config_.grid_size, config_.grid_x_count() - 1));
    grid_y = std::max(0, std::min(y / config_.grid_size, config_.grid_y_count() - 1));
}

void GridAoi::grid_to_center(int grid_x, int grid_y, int& x, int& y) const {
    x = grid_x * config_.grid_size + config_.grid_size / 2;
    y = grid_y * config_.grid_size + config_.grid_size / 2;
}

void GridAoi::get_grids_in_view(int x, int y, 
                                int& min_gx, int& min_gy, 
                                int& max_gx, int& max_gy) const {
    int center_gx, center_gy;
    pos_to_grid(x, y, center_gx, center_gy);
    
    min_gx = std::max(0, center_gx - config_.view_range);
    min_gy = std::max(0, center_gy - config_.view_range);
    max_gx = std::min(config_.grid_x_count() - 1, center_gx + config_.view_range);
    max_gy = std::min(config_.grid_y_count() - 1, center_gy + config_.view_range);
}

bool GridAoi::add_entity(const Entity& entity) {
    if (entity.id == 0) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 检查是否已存在
    if (entities_.find(entity.id) != entities_.end()) {
        CHWELL_LOG_WARN("Entity already exists: " << entity.id);
        return false;
    }
    
    // 计算格子
    int grid_x, grid_y;
    pos_to_grid(entity.x, entity.y, grid_x, grid_y);
    
    // 添加到实体列表
    entities_[entity.id] = entity;
    
    // 添加到格子
    int idx = grid_index(grid_x, grid_y);
    grids_[idx].insert(entity.id);
    
    // 记录格子
    entity_to_grid_[entity.id] = idx;
    
    // 触发进入事件（通知视野内的观察者）
    if (callback_) {
        // 获取视野内的实体
        int min_gx, min_gy, max_gx, max_gy;
        get_grids_in_view(entity.x, entity.y, min_gx, min_gy, max_gx, max_gy);
        
        for (int gy = min_gy; gy <= max_gy; ++gy) {
            for (int gx = min_gx; gx <= max_gx; ++gx) {
                int gidx = grid_index(gx, gy);
                for (uint64_t other_id : grids_[gidx]) {
                    if (other_id != entity.id) {
                        AoiEvent event;
                        event.type = EventType::ENTER;
                        event.watcher_id = other_id;
                        event.target_id = entity.id;
                        event.new_x = entity.x;
                        event.new_y = entity.y;
                        trigger_event(event);
                    }
                }
            }
        }
    }
    
    return true;
}

bool GridAoi::remove_entity(uint64_t entity_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entities_.find(entity_id);
    if (it == entities_.end()) {
        return false;
    }
    
    const Entity& entity = it->second;
    
    // 触发离开事件
    if (callback_) {
        int min_gx, min_gy, max_gx, max_gy;
        get_grids_in_view(entity.x, entity.y, min_gx, min_gy, max_gx, max_gy);
        
        for (int gy = min_gy; gy <= max_gy; ++gy) {
            for (int gx = min_gx; gx <= max_gx; ++gx) {
                int gidx = grid_index(gx, gy);
                for (uint64_t other_id : grids_[gidx]) {
                    if (other_id != entity_id) {
                        AoiEvent event;
                        event.type = EventType::LEAVE;
                        event.watcher_id = other_id;
                        event.target_id = entity_id;
                        trigger_event(event);
                    }
                }
            }
        }
    }
    
    // 从格子中移除
    auto grid_it = entity_to_grid_.find(entity_id);
    if (grid_it != entity_to_grid_.end()) {
        grids_[grid_it->second].erase(entity_id);
        entity_to_grid_.erase(grid_it);
    }
    
    // 移除实体
    entities_.erase(it);
    
    return true;
}

bool GridAoi::update_entity(uint64_t entity_id, int new_x, int new_y) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entities_.find(entity_id);
    if (it == entities_.end()) {
        return false;
    }
    
    Entity& entity = it->second;
    int old_x = entity.x;
    int old_y = entity.y;
    
    // 计算旧格子
    int old_grid_x, old_grid_y;
    pos_to_grid(old_x, old_y, old_grid_x, old_grid_y);
    
    // 计算新格子
    int new_grid_x, new_grid_y;
    pos_to_grid(new_x, new_y, new_grid_x, new_grid_y);
    
    // 更新实体位置
    entity.x = new_x;
    entity.y = new_y;
    
    // 如果格子没变，只触发移动事件
    if (old_grid_x == new_grid_x && old_grid_y == new_grid_y) {
        if (callback_) {
            AoiEvent event;
            event.type = EventType::MOVE;
            event.watcher_id = 0;  // 广播给所有观察者
            event.target_id = entity_id;
            event.old_x = old_x;
            event.old_y = old_y;
            event.new_x = new_x;
            event.new_y = new_y;
            
            // 通知格子内的观察者
            int idx = grid_index(old_grid_x, old_grid_y);
            for (uint64_t other_id : grids_[idx]) {
                if (other_id != entity_id) {
                    event.watcher_id = other_id;
                    trigger_event(event);
                }
            }
        }
        return true;
    }
    
    // 格子变化，需要计算视野变化
    
    // 获取旧视野和新视野的格子
    int old_min_gx, old_min_gy, old_max_gx, old_max_gy;
    int new_min_gx, new_min_gy, new_max_gx, new_max_gy;
    
    get_grids_in_view(old_x, old_y, old_min_gx, old_min_gy, old_max_gx, old_max_gy);
    get_grids_in_view(new_x, new_y, new_min_gx, new_min_gy, new_max_gx, new_max_gy);
    
    // 从旧格子移除，添加到新格子
    int old_idx = grid_index(old_grid_x, old_grid_y);
    int new_idx = grid_index(new_grid_x, new_grid_y);
    
    grids_[old_idx].erase(entity_id);
    grids_[new_idx].insert(entity_id);
    entity_to_grid_[entity_id] = new_idx;
    
    if (callback_) {
        // 离开旧视野的格子
        for (int gy = old_min_gy; gy <= old_max_gy; ++gy) {
            for (int gx = old_min_gx; gx <= old_max_gx; ++gx) {
                // 检查是否在新视野外
                if (gx < new_min_gx || gx > new_max_gx || 
                    gy < new_min_gy || gy > new_max_gy) {
                    int gidx = grid_index(gx, gy);
                    for (uint64_t other_id : grids_[gidx]) {
                        if (other_id != entity_id) {
                            // 实体离开观察者的视野
                            AoiEvent event;
                            event.type = EventType::LEAVE;
                            event.watcher_id = other_id;
                            event.target_id = entity_id;
                            trigger_event(event);
                            
                            // 观察者离开实体的视野
                            event.watcher_id = entity_id;
                            event.target_id = other_id;
                            trigger_event(event);
                        }
                    }
                }
            }
        }
        
        // 进入新视野的格子
        for (int gy = new_min_gy; gy <= new_max_gy; ++gy) {
            for (int gx = new_min_gx; gx <= new_max_gx; ++gx) {
                // 检查是否在旧视野外
                if (gx < old_min_gx || gx > old_max_gx || 
                    gy < old_min_gy || gy > old_max_gy) {
                    int gidx = grid_index(gx, gy);
                    for (uint64_t other_id : grids_[gidx]) {
                        if (other_id != entity_id) {
                            // 实体进入观察者的视野
                            AoiEvent event;
                            event.type = EventType::ENTER;
                            event.watcher_id = other_id;
                            event.target_id = entity_id;
                            event.new_x = new_x;
                            event.new_y = new_y;
                            trigger_event(event);
                            
                            // 观察者进入实体的视野
                            auto other_it = entities_.find(other_id);
                            if (other_it != entities_.end()) {
                                event.watcher_id = entity_id;
                                event.target_id = other_id;
                                event.new_x = other_it->second.x;
                                event.new_y = other_it->second.y;
                                trigger_event(event);
                            }
                        }
                    }
                }
            }
        }
        
        // 通知新旧视野重叠区域的观察者移动事件
        for (int gy = std::max(old_min_gy, new_min_gy); gy <= std::min(old_max_gy, new_max_gy); ++gy) {
            for (int gx = std::max(old_min_gx, new_min_gx); gx <= std::min(old_max_gx, new_max_gx); ++gx) {
                int gidx = grid_index(gx, gy);
                for (uint64_t other_id : grids_[gidx]) {
                    if (other_id != entity_id) {
                        AoiEvent event;
                        event.type = EventType::MOVE;
                        event.watcher_id = other_id;
                        event.target_id = entity_id;
                        event.old_x = old_x;
                        event.old_y = old_y;
                        event.new_x = new_x;
                        event.new_y = new_y;
                        trigger_event(event);
                    }
                }
            }
        }
    }
    
    return true;
}

bool GridAoi::get_entity(uint64_t entity_id, Entity& entity) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entities_.find(entity_id);
    if (it != entities_.end()) {
        entity = it->second;
        return true;
    }
    return false;
}

std::vector<Entity> GridAoi::get_entities_in_view(uint64_t watcher_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entities_.find(watcher_id);
    if (it == entities_.end()) {
        return {};
    }
    
    return get_entities_in_view(it->second.x, it->second.y);
}

std::vector<Entity> GridAoi::get_entities_in_view(int x, int y) const {
    std::vector<Entity> result;
    
    int min_gx, min_gy, max_gx, max_gy;
    get_grids_in_view(x, y, min_gx, min_gy, max_gx, max_gy);
    
    for (int gy = min_gy; gy <= max_gy; ++gy) {
        for (int gx = min_gx; gx <= max_gx; ++gx) {
            int gidx = grid_index(gx, gy);
            for (uint64_t eid : grids_[gidx]) {
                auto eit = entities_.find(eid);
                if (eit != entities_.end()) {
                    result.push_back(eit->second);
                }
            }
        }
    }
    
    return result;
}

std::vector<uint64_t> GridAoi::get_entity_ids_in_view(uint64_t watcher_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = entities_.find(watcher_id);
    if (it == entities_.end()) {
        return {};
    }
    
    return get_entity_ids_in_view(it->second.x, it->second.y);
}

std::vector<uint64_t> GridAoi::get_entity_ids_in_view(int x, int y) const {
    std::vector<uint64_t> result;
    
    int min_gx, min_gy, max_gx, max_gy;
    get_grids_in_view(x, y, min_gx, min_gy, max_gx, max_gy);
    
    for (int gy = min_gy; gy <= max_gy; ++gy) {
        for (int gx = min_gx; gx <= max_gx; ++gx) {
            int gidx = grid_index(gx, gy);
            for (uint64_t eid : grids_[gidx]) {
                result.push_back(eid);
            }
        }
    }
    
    return result;
}

std::vector<Entity> GridAoi::get_entities_in_grid(int grid_x, int grid_y) const {
    std::vector<Entity> result;
    
    if (!is_valid_grid(grid_x, grid_y)) {
        return result;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    int gidx = grid_index(grid_x, grid_y);
    
    for (uint64_t eid : grids_[gidx]) {
        auto it = entities_.find(eid);
        if (it != entities_.end()) {
            result.push_back(it->second);
        }
    }
    
    return result;
}

std::vector<uint64_t> GridAoi::get_entity_ids_in_grid(int grid_x, int grid_y) const {
    std::vector<uint64_t> result;
    
    if (!is_valid_grid(grid_x, grid_y)) {
        return result;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    int gidx = grid_index(grid_x, grid_y);
    
    for (uint64_t eid : grids_[gidx]) {
        result.push_back(eid);
    }
    
    return result;
}

std::vector<Entity> GridAoi::get_entities_in_grids(int center_grid_x, int center_grid_y) const {
    std::vector<Entity> result;
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    int min_gx = std::max(0, center_grid_x - config_.view_range);
    int min_gy = std::max(0, center_grid_y - config_.view_range);
    int max_gx = std::min(config_.grid_x_count() - 1, center_grid_x + config_.view_range);
    int max_gy = std::min(config_.grid_y_count() - 1, center_grid_y + config_.view_range);
    
    for (int gy = min_gy; gy <= max_gy; ++gy) {
        for (int gx = min_gx; gx <= max_gx; ++gx) {
            int gidx = grid_index(gx, gy);
            for (uint64_t eid : grids_[gidx]) {
                auto it = entities_.find(eid);
                if (it != entities_.end()) {
                    result.push_back(it->second);
                }
            }
        }
    }
    
    return result;
}

void GridAoi::trigger_event(const AoiEvent& event) {
    if (callback_) {
        try {
            callback_(event);
        } catch (const std::exception& e) {
            CHWELL_LOG_ERROR("AOI callback exception: " << e.what());
        }
    }
}

int GridAoi::total_entities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(entities_.size());
}

int GridAoi::entity_count_in_grid(int grid_x, int grid_y) const {
    if (!is_valid_grid(grid_x, grid_y)) {
        return 0;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    int gidx = grid_index(grid_x, grid_y);
    return static_cast<int>(grids_[gidx].size());
}

//=============================================================================
// CrossListAoi 实现
//=============================================================================

CrossListAoi::CrossListAoi(const Config& config)
    : config_(config), head_x_(nullptr), head_y_(nullptr) {
    CHWELL_LOG_INFO("CrossListAoi initialized: map=" << config_.map_width << "x" << config_.map_height
                  << ", view_range=" << config_.view_range);
}

bool CrossListAoi::add_entity(const Entity& entity) {
    if (entity.id == 0) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (nodes_.find(entity.id) != nodes_.end()) {
        return false;
    }
    
    auto node = std::make_unique<Node>();
    node->entity = entity;
    
    insert_to_x_list(node.get());
    insert_to_y_list(node.get());
    
    nodes_[entity.id] = std::move(node);
    
    return true;
}

void CrossListAoi::insert_to_x_list(Node* node) {
    if (!head_x_) {
        head_x_ = node;
        return;
    }
    
    Node* prev = nullptr;
    Node* curr = head_x_;
    
    while (curr && curr->entity.x < node->entity.x) {
        prev = curr;
        curr = curr->next_x;
    }
    
    node->prev_x = prev;
    node->next_x = curr;
    
    if (prev) {
        prev->next_x = node;
    } else {
        head_x_ = node;
    }
    
    if (curr) {
        curr->prev_x = node;
    }
}

void CrossListAoi::insert_to_y_list(Node* node) {
    if (!head_y_) {
        head_y_ = node;
        return;
    }
    
    Node* prev = nullptr;
    Node* curr = head_y_;
    
    while (curr && curr->entity.y < node->entity.y) {
        prev = curr;
        curr = curr->next_y;
    }
    
    node->prev_y = prev;
    node->next_y = curr;
    
    if (prev) {
        prev->next_y = node;
    } else {
        head_y_ = node;
    }
    
    if (curr) {
        curr->prev_y = node;
    }
}

void CrossListAoi::remove_from_lists(Node* node) {
    // 从 X 链表移除
    if (node->prev_x) {
        node->prev_x->next_x = node->next_x;
    } else {
        head_x_ = node->next_x;
    }
    if (node->next_x) {
        node->next_x->prev_x = node->prev_x;
    }
    
    // 从 Y 链表移除
    if (node->prev_y) {
        node->prev_y->next_y = node->next_y;
    } else {
        head_y_ = node->next_y;
    }
    if (node->next_y) {
        node->next_y->prev_y = node->prev_y;
    }
}

bool CrossListAoi::remove_entity(uint64_t entity_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = nodes_.find(entity_id);
    if (it == nodes_.end()) {
        return false;
    }
    
    remove_from_lists(it->second.get());
    nodes_.erase(it);
    
    return true;
}

bool CrossListAoi::update_entity(uint64_t entity_id, int new_x, int new_y) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = nodes_.find(entity_id);
    if (it == nodes_.end()) {
        return false;
    }
    
    Node* node = it->second.get();
    
    // 从链表移除并重新插入
    remove_from_lists(node);
    
    node->entity.x = new_x;
    node->entity.y = new_y;
    
    insert_to_x_list(node);
    insert_to_y_list(node);
    
    return true;
}

std::vector<Entity> CrossListAoi::get_entities_in_view(uint64_t watcher_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = nodes_.find(watcher_id);
    if (it == nodes_.end()) {
        return {};
    }
    
    return get_entities_in_view(it->second->entity.x, it->second->entity.y);
}

std::vector<Entity> CrossListAoi::get_entities_in_view(int x, int y) const {
    std::vector<Entity> result;
    
    int min_x = x - config_.view_range;
    int max_x = x + config_.view_range;
    int min_y = y - config_.view_range;
    int max_y = y + config_.view_range;
    
    // 遍历 X 链表
    for (Node* curr = head_x_; curr; curr = curr->next_x) {
        if (curr->entity.x > max_x) break;
        if (curr->entity.x >= min_x && 
            curr->entity.y >= min_y && curr->entity.y <= max_y) {
            result.push_back(curr->entity);
        }
    }
    
    return result;
}

int CrossListAoi::total_entities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(nodes_.size());
}

} // namespace aoi
} // namespace chwell