#pragma once

#include <vector>
#include <list>
#include <functional>
#include <chrono>
#include <mutex>
#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>

namespace chwell {
namespace core {

// 定时器回调类型
using TimerCallback = std::function<void()>;

/**
 * @brief 定时器句柄，用于取消定时器
 *
 * P2 改进：内含层号+槽号定位信息，支持 O(1) 精准移除
 */
class TimerHandle {
public:
    TimerHandle() : id_(0), valid_(false), layer_(-1), slot_(-1) {}
    explicit TimerHandle(uint64_t id, int layer = -1, int slot = -1)
        : id_(id), valid_(true), layer_(layer), slot_(slot) {}

    uint64_t id() const { return id_; }
    bool valid() const { return valid_; }
    void invalidate() { valid_ = false; layer_ = -1; slot_ = -1; }

    // 🆕 定位信息：用于 O(1) 取消
    int layer() const { return layer_; }
    int slot() const { return slot_; }

    bool operator==(const TimerHandle& other) const { return id_ == other.id_; }
    bool operator!=(const TimerHandle& other) const { return id_ != other.id_; }

private:
    uint64_t id_;
    bool valid_;
    int layer_;   // 🆕 所在层级
    int slot_;    // 🆕 所在槽位
};

/**
 * @brief 时间轮定时器
 *
 * 支持：
 * - 一次性定时器和重复定时器
 * - 多层时间轮，支持大范围延迟
 * - O(1) 添加、O(1) 取消（侵入式链表 + 定位信息）
 *
 * P2 改进：
 * - TimerTask 包含链表节点信息（prev/next 迭代器）
 * - TimerHandle 包含层号+槽号，cancel 时直接定位
 * - 取消操作从"标记 cancelled"升级为"直接从链表移除"
 */
class TimerWheel {
public:
    // 定时器任务
    struct TimerTask {
        uint64_t id;              // 定时器ID
        TimerCallback callback;   // 回调函数
        int64_t expire_time;      // 到期时间（毫秒）
        int interval;             // 重复间隔（0表示一次性）
        int rounds_left;          // 剩余轮数（用于多层时间轮）
        bool cancelled;           // 是否已取消

        // 🆕 链表定位信息（用于 O(1) 移除）
        int layer;                // 所在层级
        int slot;                 // 所在槽位
        typename std::list<std::shared_ptr<TimerTask>>::iterator list_iter;  // 🆕 链表迭代器

        TimerTask()
            : id(0), expire_time(0), interval(0),
              rounds_left(0), cancelled(false), layer(-1), slot(-1) {}
    };

    // 构造函数
    explicit TimerWheel(int tick_ms = 100, int wheel_size = 60, int layers = 4);

    ~TimerWheel();

    // 启动时间轮（后台线程）
    void start();

    // 停止时间轮
    void stop();

    // 添加一次性定时器，返回定时器句柄
    TimerHandle add_timer(int delay_ms, TimerCallback callback);

    // 添加重复定时器，返回定时器句柄
    TimerHandle add_repeat_timer(int interval_ms, TimerCallback callback);

    /**
     * @brief 取消定时器（O(1) 优化）
     *
     * 如果 TimerHandle 含有层号+槽号，直接定位到链表并用迭代器移除。
     * 否则 fallback 到 task_map_ 查找。
     */
    void cancel_timer(TimerHandle& handle);

    // 检查定时器是否有效
    bool is_timer_valid(const TimerHandle& handle) const;

    // 手动驱动时间轮（用于测试或自定义驱动）
    void tick();

    // 获取下一个到期时间（毫秒），无定时器返回-1
    int64_t get_next_expire_time() const;

    // 获取当前时间（毫秒）
    static int64_t current_time_ms();

private:
    // 时间轮槽
    struct WheelSlot {
        std::list<std::shared_ptr<TimerTask>> tasks;
    };

    // 单层时间轮
    struct Wheel {
        std::vector<WheelSlot> slots;
        int current_slot;
        int tick_ms;
        int wheel_size;
        int total_ms;

        Wheel(int size, int tick)
            : slots(size), current_slot(0), tick_ms(tick),
              wheel_size(size), total_ms(size * tick) {}
    };

    // 将任务添加到合适的层级，返回 (layer, slot)
    std::pair<int, int> add_task_to_wheel(std::shared_ptr<TimerTask> task);

    // 处理单个槽的任务
    void process_slot(int layer, int slot, std::vector<std::shared_ptr<TimerTask>>& due_tasks);

    // 级联处理
    void cascade(int layer);

    // 运行循环
    void run_loop();

    // 生成唯一ID
    uint64_t generate_id();

    std::vector<Wheel> wheels_;
    mutable std::mutex mutex_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::atomic<uint64_t> next_id_;

    // 用于快速查找（fallback 路径）
    mutable std::unordered_map<uint64_t, std::weak_ptr<TimerTask>> task_map_;
};

/**
 * @brief 定时器管理器（单例模式）
 */
class TimerManager {
public:
    static TimerManager& instance() {
        static TimerManager inst;
        return inst;
    }

    void init(int tick_ms = 100, int wheel_size = 60, int layers = 4) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!wheel_) {
            wheel_.reset(new TimerWheel(tick_ms, wheel_size, layers));
            wheel_->start();
        }
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_) {
            wheel_->stop();
            wheel_.reset();
        }
    }

    TimerHandle add_timer(int delay_ms, TimerCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_) {
            return wheel_->add_timer(delay_ms, std::move(callback));
        }
        return TimerHandle();
    }

    TimerHandle add_repeat_timer(int interval_ms, TimerCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_) {
            return wheel_->add_repeat_timer(interval_ms, std::move(callback));
        }
        return TimerHandle();
    }

    void cancel_timer(TimerHandle& handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wheel_) {
            wheel_->cancel_timer(handle);
        }
    }

    TimerWheel* get_wheel() {
        std::lock_guard<std::mutex> lock(mutex_);
        return wheel_.get();
    }

private:
    TimerManager() : wheel_(nullptr) {}
    ~TimerManager() { shutdown(); }

    std::mutex mutex_;
    std::unique_ptr<TimerWheel> wheel_;
};

} // namespace core
} // namespace chwell
