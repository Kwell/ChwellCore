#include "chwell/core/timer_wheel.h"
#include "chwell/core/logger.h"
#include <algorithm>

namespace chwell {
namespace core {

TimerWheel::TimerWheel(int tick_ms, int wheel_size, int layers)
    : running_(false), next_id_(1) {
    // 创建多层时间轮
    // 第0层: tick_ms * wheel_size
    // 第1层: tick_ms * wheel_size * wheel_size
    // 第2层: tick_ms * wheel_size^3
    // ...
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
    if (running_.exchange(true)) {
        return; // 已经在运行
    }
    
    thread_ = std::thread([this]() {
        run_loop();
    });
    
    CHWELL_LOG_INFO("TimerWheel started");
}

void TimerWheel::stop() {
    if (!running_.exchange(false)) {
        return; // 已经停止
    }
    
    if (thread_.joinable()) {
        thread_.join();
    }
    
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
    task->interval = 0;  // 一次性
    task->cancelled = false;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        add_task_to_wheel(task);
        task_map_[task->id] = task;
    }
    
    return TimerHandle(task->id);
}

TimerHandle TimerWheel::add_repeat_timer(int interval_ms, TimerCallback callback) {
    if (interval_ms <= 0 || !callback) {
        return TimerHandle();
    }
    
    auto task = std::make_shared<TimerTask>();
    task->id = generate_id();
    task->callback = std::move(callback);
    task->expire_time = current_time_ms() + interval_ms;
    task->interval = interval_ms;  // 重复间隔
    task->cancelled = false;
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        add_task_to_wheel(task);
        task_map_[task->id] = task;
    }
    
    return TimerHandle(task->id);
}

void TimerWheel::cancel_timer(TimerHandle& handle) {
    if (!handle.valid()) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
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
    if (!handle.valid()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = task_map_.find(handle.id());
    if (it != task_map_.end()) {
        auto task = it->second.lock();
        return task && !task->cancelled;
    }
    return false;
}

void TimerWheel::add_task_to_wheel(std::shared_ptr<TimerTask> task) {
    int64_t delay = task->expire_time - current_time_ms();
    if (delay <= 0) {
        delay = 1; // 至少1ms
    }
    
    // 找到合适的层级
    for (size_t i = 0; i < wheels_.size(); ++i) {
        Wheel& wheel = wheels_[i];
        
        if (delay < wheel.total_ms || i == wheels_.size() - 1) {
            // 放在这一层
            int ticks = static_cast<int>(delay / wheel.tick_ms);
            int slot = (wheel.current_slot + ticks) % wheel.wheel_size;
            
            task->rounds_left = ticks / wheel.wheel_size;
            
            wheel.slots[slot].tasks.push_back(task);
            return;
        }
    }
    
    // 超出最大范围，放在最后一层
    Wheel& last_wheel = wheels_.back();
    int ticks = static_cast<int>(delay / last_wheel.tick_ms);
    int slot = (last_wheel.current_slot + ticks) % last_wheel.wheel_size;
    task->rounds_left = ticks / last_wheel.wheel_size;
    last_wheel.slots[slot].tasks.push_back(task);
}

void TimerWheel::process_slot(int layer, int slot) {
    Wheel& wheel = wheels_[layer];
    auto& tasks = wheel.slots[slot].tasks;
    
    auto it = tasks.begin();
    while (it != tasks.end()) {
        auto& task = *it;
        
        if (task->cancelled) {
            // 已取消，移除
            it = tasks.erase(it);
            continue;
        }
        
        if (task->rounds_left > 0) {
            // 还需要转几轮
            --task->rounds_left;
            ++it;
            continue;
        }
        
        // 到期执行
        TimerCallback callback = task->callback;
        
        if (task->interval > 0) {
            // 重复定时器，重新添加
            task->expire_time = current_time_ms() + task->interval;
            auto new_task = std::make_shared<TimerTask>(*task);
            add_task_to_wheel(new_task);
            
            // 更新map中的指针
            {
                std::lock_guard<std::mutex> lock(mutex_);
                task_map_[task->id] = new_task;
            }
        }
        
        // 从列表中移除
        it = tasks.erase(it);
        
        // 执行回调（注意：不要在锁内执行）
        if (callback) {
            try {
                callback();
            } catch (const std::exception& e) {
                CHWELL_LOG_ERROR("Timer callback exception: " << e.what());
            } catch (...) {
                CHWELL_LOG_ERROR("Timer callback unknown exception");
            }
        }
    }
}

void TimerWheel::cascade(int layer) {
    if (layer <= 0 || layer >= static_cast<int>(wheels_.size())) {
        return;
    }
    
    Wheel& upper = wheels_[layer];
    int slot = upper.current_slot;
    
    auto& tasks = upper.slots[slot].tasks;
    auto tasks_to_move = std::move(tasks); // 取走所有任务
    tasks.clear();
    
    // 将任务重新添加到下层
    for (auto& task : tasks_to_move) {
        if (task->cancelled) {
            continue;
        }
        
        // 重新计算延迟
        int64_t delay = task->expire_time - current_time_ms();
        if (delay <= 0) {
            // 已过期，放到第0层的当前槽
            wheels_[0].slots[wheels_[0].current_slot].tasks.push_back(task);
        } else {
            add_task_to_wheel(task);
        }
    }
}

void TimerWheel::tick() {
    // 处理第0层当前槽
    process_slot(0, wheels_[0].current_slot);
    
    // 推进第0层
    wheels_[0].current_slot = (wheels_[0].current_slot + 1) % wheels_[0].wheel_size;
    
    // 检查是否需要级联
    if (wheels_[0].current_slot == 0) {
        // 第0层转完一圈，级联第1层
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

void TimerWheel::run_loop() {
    while (running_) {
        auto start = std::chrono::steady_clock::now();
        
        tick();
        
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto sleep_time = std::chrono::milliseconds(wheels_[0].tick_ms) - elapsed;
        
        if (sleep_time.count() > 0) {
            std::this_thread::sleep_for(sleep_time);
        }
    }
}

int64_t TimerWheel::get_next_expire_time() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 扫描第0层找到最近的到期时间
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
        if (min_expire >= 0) {
            break; // 找到了
        }
    }
    
    return min_expire;
}

} // namespace core
} // namespace chwell