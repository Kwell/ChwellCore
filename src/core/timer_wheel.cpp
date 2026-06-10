#include "chwell/core/timer_wheel.h"
#include "chwell/core/logger.h"
#include <algorithm>

namespace chwell {
namespace core {

TimerWheel::TimerWheel(int tick_ms, int wheel_size, int layers)
    : running_(false), next_id_(1) {
    int current_tick = tick_ms;
    for (int i = 0; i < layers; ++i) {
        wheels_.emplace_back(wheel_size, current_tick);
        current_tick *= wheel_size;
    }
}

TimerWheel::~TimerWheel() {
    stop();
}

void TimerWheel::start() {
    if (running_.exchange(true)) return;

    for (auto& wheel : wheels_) {
        wheel.current_slot = 0;
    }

    thread_ = std::thread([this]() { run_loop(); });
    CHWELL_LOG_INFO("TimerWheel started");
}

void TimerWheel::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    CHWELL_LOG_INFO("TimerWheel stopped");
}

uint64_t TimerWheel::generate_id() {
    return next_id_.fetch_add(1, std::memory_order_relaxed);
}

int64_t TimerWheel::current_time_ms() {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

TimerHandle TimerWheel::add_timer(int delay_ms, TimerCallback callback) {
    if (delay_ms <= 0 || !callback) {
        return TimerHandle();
    }

    auto task = std::make_shared<TimerTask>();
    task->id = generate_id();
    task->callback = std::move(callback);
    task->expire_time = current_time_ms() + delay_ms;
    task->interval = 0;
    task->cancelled = false;

    int layer = -1, slot = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto loc = add_task_to_wheel(task);
        layer = loc.first;
        slot = loc.second;
        task_map_[task->id] = task;
    }

    // 🆕 返回含定位信息的 Handle
    return TimerHandle(task->id, layer, slot);
}

TimerHandle TimerWheel::add_repeat_timer(int interval_ms, TimerCallback callback) {
    if (interval_ms <= 0 || !callback) {
        return TimerHandle();
    }

    auto task = std::make_shared<TimerTask>();
    task->id = generate_id();
    task->callback = std::move(callback);
    task->expire_time = current_time_ms() + interval_ms;
    task->interval = interval_ms;
    task->cancelled = false;

    int layer = -1, slot = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto loc = add_task_to_wheel(task);
        layer = loc.first;
        slot = loc.second;
        task_map_[task->id] = task;
    }

    return TimerHandle(task->id, layer, slot);
}

void TimerWheel::cancel_timer(TimerHandle& handle) {
    if (!handle.valid()) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // 🆕 O(1) 路径：通过 TimerHandle 的定位信息直接移除
    int h_layer = handle.layer();
    int h_slot = handle.slot();

    if (h_layer >= 0 && h_layer < static_cast<int>(wheels_.size()) &&
        h_slot >= 0 && h_slot < wheels_[h_layer].wheel_size) {

        auto& tasks_list = wheels_[h_layer].slots[h_slot].tasks;

        // 尝试用迭代器直接移除（如果 task 还持有有效的迭代器）
        // 先通过 task_map_ 找到 task
        auto map_it = task_map_.find(handle.id());
        if (map_it != task_map_.end()) {
            auto task = map_it->second.lock();
            if (task && !task->cancelled) {
                task->cancelled = true;

                // 🆕 用迭代器 O(1) 移除（如果迭代器仍有效）
                // std::list 的迭代器在插入/删除其他元素时不会失效
                // 但如果 task 已经被 process_slot 移走了，迭代器就无效了
                // 安全做法：遍历该槽查找（最坏 O(N) 但 N 通常很小）
                for (auto it = tasks_list.begin(); it != tasks_list.end(); ++it) {
                    if ((*it)->id == handle.id()) {
                        tasks_list.erase(it);
                        break;
                    }
                }
            }
            task_map_.erase(map_it);
        }

        handle.invalidate();
        return;
    }

    // Fallback：只通过 task_map_ 标记取消
    auto it = task_map_.find(handle.id());
    if (it != task_map_.end()) {
        auto task = it->second.lock();
        if (task) {
            task->cancelled = true;
        }
        task_map_.erase(it);
    }
    handle.invalidate();
}

bool TimerWheel::is_timer_valid(const TimerHandle& handle) const {
    if (!handle.valid()) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = task_map_.find(handle.id());
    if (it != task_map_.end()) {
        auto task = it->second.lock();
        return task && !task->cancelled;
    }
    return false;
}

std::pair<int, int> TimerWheel::add_task_to_wheel(std::shared_ptr<TimerTask> task) {
    int64_t delay = task->expire_time - current_time_ms();
    if (delay <= 0) delay = 1;

    int target_layer = -1;
    int target_slot = -1;

    for (size_t i = 0; i < wheels_.size(); ++i) {
        Wheel& wheel = wheels_[i];

        if (delay < wheel.total_ms || i == wheels_.size() - 1) {
            int ticks = static_cast<int>(delay / wheel.tick_ms);
            int slot = (wheel.current_slot + ticks) % wheel.wheel_size;

            task->rounds_left = ticks / wheel.wheel_size;

            // 🆕 记录定位信息
            task->layer = static_cast<int>(i);
            task->slot = slot;

            // 🆕 记录迭代器
            wheel.slots[slot].tasks.push_back(task);
            task->list_iter = std::prev(wheel.slots[slot].tasks.end());

            target_layer = static_cast<int>(i);
            target_slot = slot;
            return {target_layer, target_slot};
        }
    }

    // 超出最大范围，放在最后一层
    Wheel& last_wheel = wheels_.back();
    int ticks = static_cast<int>(delay / last_wheel.tick_ms);
    int slot = (last_wheel.current_slot + ticks) % last_wheel.wheel_size;
    task->rounds_left = ticks / last_wheel.wheel_size;

    task->layer = static_cast<int>(wheels_.size() - 1);
    task->slot = slot;

    last_wheel.slots[slot].tasks.push_back(task);
    task->list_iter = std::prev(last_wheel.slots[slot].tasks.end());

    return {static_cast<int>(wheels_.size() - 1), slot};
}

void TimerWheel::process_slot(int layer, int slot, std::vector<std::shared_ptr<TimerTask>>& due_tasks) {
    Wheel& wheel = wheels_[layer];
    auto& tasks = wheel.slots[slot].tasks;

    auto it = tasks.begin();
    while (it != tasks.end()) {
        auto& task = *it;

        if (task->cancelled) {
            it = tasks.erase(it);
            continue;
        }

        if (task->rounds_left > 0) {
            --task->rounds_left;
            ++it;
            continue;
        }

        due_tasks.push_back(task);
        it = tasks.erase(it);
    }
}

void TimerWheel::cascade(int layer) {
    if (layer <= 0 || layer >= static_cast<int>(wheels_.size())) return;

    Wheel& upper = wheels_[layer];
    int slot = upper.current_slot;

    auto& tasks = upper.slots[slot].tasks;
    auto tasks_to_move = std::move(tasks);
    tasks.clear();

    for (auto& task : tasks_to_move) {
        if (task->cancelled) continue;

        int64_t delay = task->expire_time - current_time_ms();
        if (delay <= 0) {
            task->layer = 0;
            task->slot = wheels_[0].current_slot;
            wheels_[0].slots[wheels_[0].current_slot].tasks.push_back(task);
            task->list_iter = std::prev(wheels_[0].slots[wheels_[0].current_slot].tasks.end());
        } else {
            add_task_to_wheel(task);
        }
    }
}

void TimerWheel::tick() {
    std::vector<std::shared_ptr<TimerTask>> due_tasks;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        process_slot(0, wheels_[0].current_slot, due_tasks);

        wheels_[0].current_slot = (wheels_[0].current_slot + 1) % wheels_[0].wheel_size;

        if (wheels_[0].current_slot == 0) {
            cascade(1);
            wheels_[1].current_slot = (wheels_[1].current_slot + 1) % wheels_[1].wheel_size;

            if (wheels_[1].current_slot == 0 && wheels_.size() > 2) {
                cascade(2);
                wheels_[2].current_slot = (wheels_[2].current_slot + 1) % wheels_[2].wheel_size;

                if (wheels_[2].current_slot == 0 && wheels_.size() > 3) {
                    cascade(3);
                    wheels_[3].current_slot = (wheels_[3].current_slot + 1) % wheels_[3].wheel_size;
                }
            }
        }
    }

    // 在锁外执行回调
    for (auto& task : due_tasks) {
        if (task->callback) {
            try {
                task->callback();
            } catch (const std::exception& e) {
                CHWELL_LOG_ERROR("Timer callback exception: " << e.what());
            } catch (...) {
                CHWELL_LOG_ERROR("Timer callback unknown exception");
            }
        }

        // 重复定时器重新添加
        if (task->interval > 0 && !task->cancelled) {
            // 🆕 重新计算 expire_time，基于当前时间而非上次到期时间
            // 这样即使回调执行耗时较长，也不会导致重复定时器堆积
            task->expire_time = current_time_ms() + task->interval;
            auto new_task = std::make_shared<TimerTask>(*task);
            // 重置无效的迭代器（add_task_to_wheel 会重新设置）
            new_task->list_iter = {};
            new_task->layer = -1;
            new_task->slot = -1;
            {
                std::lock_guard<std::mutex> lock2(mutex_);
                add_task_to_wheel(new_task);
                task_map_[task->id] = new_task;
            }
        }
    }
}

void TimerWheel::run_loop() {
    auto next_tick = std::chrono::steady_clock::now();
    while (running_) {
        next_tick += std::chrono::milliseconds(wheels_[0].tick_ms);
        tick();
        std::this_thread::sleep_until(next_tick);
    }
}

int64_t TimerWheel::get_next_expire_time() const {
    std::lock_guard<std::mutex> lock(mutex_);

    int64_t min_expire = -1;
    const Wheel& wheel = wheels_[0];

    for (int i = 0; i < wheel.wheel_size; ++i) {
        int slot = (wheel.current_slot + i) % wheel.wheel_size;
        for (const auto& task : wheel.slots[slot].tasks) {
            if (!task->cancelled) {
                if (min_expire < 0 || task->expire_time < min_expire) {
                    min_expire = task->expire_time;
                }
            }
        }
        if (min_expire >= 0) break;
    }

    return min_expire;
}

} // namespace core
} // namespace chwell
