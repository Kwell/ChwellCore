#pragma once

#include <vector>
#include <string_view>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>

#include "chwell/net/tcp_connection.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace net {

/**
 * @brief 逻辑消息：从 Reactor 线程投递到 Logic Thread 的消息
 *
 * 设计要点：
 * - conn_raw: 裸指针，Logic Thread 内部使用，无引用计数开销
 * - data: 消息体（拷贝一次，脱离 Reactor 线程生命周期）
 * - conn_guard: 保持连接存活（shared_ptr 只在此处持有）
 * - task: 自定义任务（用于 RPC/DB 回调 marshalling 回逻辑线程）
 */
struct LogicMessage {
    enum Type : uint8_t {
        kMessage = 0,
        kDisconnect = 1,
        kTask = 2  // 🆕 自定义任务（跨线程回调 marshalling）
    };

    Type type;
    TcpConnection* conn_raw;          // 裸指针，Logic Thread 内部用
    std::vector<char> data;           // 消息体副本
    std::shared_ptr<void> conn_guard; // 持有 shared_ptr，防止连接在队列中被释放
    std::function<void()> task;       // 🆕 自定义任务

    LogicMessage() : type(kMessage), conn_raw(nullptr) {}

    // 构造消息事件
    static LogicMessage make_message(const TcpConnectionPtr& conn,
                                     std::string_view sv) {
        LogicMessage msg;
        msg.type = kMessage;
        msg.conn_raw = conn.get();
        msg.data.assign(sv.begin(), sv.end());
        msg.conn_guard = conn;
        return msg;
    }

    // 构造断连事件
    static LogicMessage make_disconnect(const TcpConnectionPtr& conn) {
        LogicMessage msg;
        msg.type = kDisconnect;
        msg.conn_raw = conn.get();
        msg.conn_guard = conn;
        return msg;
    }

    // 🆕 构造自定义任务事件（用于跨线程回调 marshalling）
    static LogicMessage make_task(std::function<void()> task) {
        LogicMessage msg;
        msg.type = kTask;
        msg.conn_raw = nullptr;
        msg.task = std::move(task);
        return msg;
    }
};

/**
 * @brief Logic Thread：单线程消费逻辑消息
 *
 * 架构角色：
 * - Reactor 线程只负责 I/O 和解包
 * - 解完包后将消息投递到 LogicThread 的环形队列
 * - LogicThread 单线程按序消费，保证业务逻辑线程安全
 * - 跨线程回调（DB/RPC）通过 post_task() 投递回 LogicThread 执行
 *
 * 性能特征：
 * - 环形队列，无动态内存分配
 * - 批量处理，每批最多 max_batch_ 条消息
 * - 空闲时 condition_variable 等待，不消耗 CPU
 *
 * 安全特性（P0）：
 * - 阻塞监控：每条消息处理耗时超过 slow_threshold_ms_ 触发告警
 * - 帧耗时监控：整批处理超过 frame_threshold_ms_ 触发告警
 * - 处理统计：可查询 slow_count / blocked_count
 */
class LogicThread {
public:
    using MessageHandler = std::function<void(const TcpConnectionPtr&, std::string_view)>;
    using DisconnectHandler = std::function<void(const TcpConnectionPtr&)>;
    using StallHandler = std::function<void(int64_t elapsed_ms, const char* phase)>;

    explicit LogicThread(size_t queue_capacity = 65536)
        : running_(false)
        , queue_capacity_(queue_capacity)
        , queue_(new LogicMessage[queue_capacity])
        , write_idx_(0)
        , read_idx_(0)
        , max_batch_(256)
        , spin_count_(0)
        , max_spin_(100)
        , slow_threshold_ms_(50)
        , frame_threshold_ms_(100)
        , slow_count_(0)
        , blocked_count_(0)
        , total_processed_(0) {}

    ~LogicThread() {
        stop();
        delete[] queue_;
    }

    LogicThread(const LogicThread&) = delete;
    LogicThread& operator=(const LogicThread&) = delete;

    // 设置回调（启动前调用）
    void set_message_handler(MessageHandler cb) { msg_handler_ = std::move(cb); }
    void set_disconnect_handler(DisconnectHandler cb) { disc_handler_ = std::move(cb); }

    // 🆕 设置阻塞告警回调（可选）
    void set_stall_handler(StallHandler cb) { stall_handler_ = std::move(cb); }

    // 🆕 设置慢回调阈值（毫秒），超过则触发 stall_handler
    void set_slow_threshold_ms(int64_t ms) { slow_threshold_ms_ = ms; }

    // 🆕 设置帧超时阈值（毫秒），一帧处理超过则触发告警
    void set_frame_threshold_ms(int64_t ms) { frame_threshold_ms_ = ms; }

    // 设置批量处理上限
    void set_max_batch(size_t n) { max_batch_ = n; }

    // 从 Reactor 线程投递消息
    bool post(const TcpConnectionPtr& conn, std::string_view data) {
        std::lock_guard<std::mutex> lock(post_mutex_);

        size_t next = (write_idx_ + 1) % queue_capacity_;
        if (next == read_idx_.load(std::memory_order_acquire)) {
            CHWELL_LOG_WARN("LogicThread queue full, dropping message from fd="
                            << (conn ? conn->native_handle() : -1));
            return false;
        }

        queue_[write_idx_] = LogicMessage::make_message(conn, data);
        write_idx_ = next;

        notify();
        return true;
    }

    // 投递断连事件
    bool post_disconnect(const TcpConnectionPtr& conn) {
        std::lock_guard<std::mutex> lock(post_mutex_);

        size_t next = (write_idx_ + 1) % queue_capacity_;
        if (next == read_idx_.load(std::memory_order_acquire)) {
            return false;
        }

        queue_[write_idx_] = LogicMessage::make_disconnect(conn);
        write_idx_ = next;

        notify();
        return true;
    }

    /**
     * @brief 🆕 投递自定义任务（跨线程回调 Marshalling）
     *
     * 用于 DB/RPC/Timer 回调将工作投递回逻辑线程串行执行，
     * 避免回调线程直接修改游戏状态导致数据竞争。
     *
     * 示例：
     *   storage->async_get(key, [logic, player_id](Result r) {
     *       // 此处在 DB 线程，不能直接改 player
     *       logic->post_task([player_id, r]() {
     *           // 此处在 Logic Thread，安全
     *           auto* p = find_player(player_id);
     *           if (p) p->set_gold(r.value);
     *       });
     *   });
     */
    bool post_task(std::function<void()> task) {
        if (!task) return false;

        std::lock_guard<std::mutex> lock(post_mutex_);

        size_t next = (write_idx_ + 1) % queue_capacity_;
        if (next == read_idx_.load(std::memory_order_acquire)) {
            CHWELL_LOG_WARN("LogicThread queue full, dropping task");
            return false;
        }

        queue_[write_idx_] = LogicMessage::make_task(std::move(task));
        write_idx_ = next;

        notify();
        return true;
    }

    // 启动 Logic Thread
    void start() {
        if (running_.exchange(true)) return;

        thread_ = std::thread([this]() {
            CHWELL_LOG_INFO("LogicThread started (slow_threshold="
                            << slow_threshold_ms_ << "ms, frame_threshold="
                            << frame_threshold_ms_ << "ms)");
            run_loop();
            CHWELL_LOG_INFO("LogicThread stopped (processed="
                            << total_processed_.load() << ", slow="
                            << slow_count_.load() << ", blocked="
                            << blocked_count_.load() << ")");
        });
    }

    // 停止 Logic Thread
    void stop() {
        if (!running_.exchange(false)) return;

        notify();
        if (thread_.joinable()) thread_.join();
    }

    /**
     * @brief 🆕 排空所有等待消息（用于 graceful shutdown）
     *
     * 调用方应先停止 Reactor 不再投递新消息，然后调用此方法等待队列消化。
     * 超时返回 false。
     */
    bool drain(int timeout_ms = 5000) {
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(timeout_ms);

        while (std::chrono::steady_clock::now() < deadline) {
            if (pending_count() == 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return pending_count() == 0;
    }

    bool is_running() const { return running_; }

    // 队列中等待处理的消息数（近似值）
    size_t pending_count() const {
        size_t w = write_idx_;
        size_t r = read_idx_.load(std::memory_order_relaxed);
        return (w >= r) ? (w - r) : (queue_capacity_ - r + w);
    }

    // 🆕 统计信息
    uint64_t total_processed() const { return total_processed_.load(); }
    uint64_t slow_count() const { return slow_count_.load(); }
    uint64_t blocked_count() const { return blocked_count_.load(); }

private:
    void notify() {
        spin_count_.store(0, std::memory_order_release);
        cv_.notify_one();
    }

    void run_loop() {
        while (running_) {
            size_t processed = 0;
            auto frame_start = std::chrono::steady_clock::now();

            while (processed < max_batch_) {
                size_t r = read_idx_.load(std::memory_order_relaxed);
                if (r == write_idx_) break;

                LogicMessage& msg = queue_[r];

                TcpConnectionPtr conn;
                if (msg.conn_guard) {
                    conn = std::static_pointer_cast<TcpConnection>(msg.conn_guard);
                }

                // 🆕 单条消息耗时监控
                auto msg_start = std::chrono::steady_clock::now();

                if (msg.type == LogicMessage::kMessage && msg_handler_) {
                    std::string_view sv(msg.data.data(), msg.data.size());
                    msg_handler_(conn, sv);
                } else if (msg.type == LogicMessage::kDisconnect && disc_handler_) {
                    disc_handler_(conn);
                } else if (msg.type == LogicMessage::kTask && msg.task) {
                    // 🆕 自定义任务（跨线程回调 marshalling）
                    msg.task();
                }

                // 🆕 慢消息检测
                auto msg_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - msg_start).count();
                if (msg_elapsed >= slow_threshold_ms_) {
                    slow_count_.fetch_add(1, std::memory_order_relaxed);
                    CHWELL_LOG_WARN("LogicThread slow message: " << msg_elapsed
                                    << "ms (type=" << (int)msg.type << ")");
                    if (stall_handler_) {
                        stall_handler_(msg_elapsed, "message");
                    }
                }

                // 清空消息，释放资源
                msg = LogicMessage();
                read_idx_.store((r + 1) % queue_capacity_, std::memory_order_release);
                ++processed;
                total_processed_.fetch_add(1, std::memory_order_relaxed);
            }

            // 🆕 帧耗时监控
            if (processed > 0) {
                auto frame_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - frame_start).count();
                if (frame_elapsed >= frame_threshold_ms_) {
                    blocked_count_.fetch_add(1, std::memory_order_relaxed);
                    CHWELL_LOG_ERROR("LogicThread frame blocked: " << frame_elapsed
                                     << "ms (processed=" << processed << " messages)");
                    if (stall_handler_) {
                        stall_handler_(frame_elapsed, "frame");
                    }
                }

                spin_count_.store(0, std::memory_order_relaxed);
                continue;
            }

            wait_for_messages();
        }

        drain_remaining();
    }

    void wait_for_messages() {
        uint32_t spins = spin_count_.fetch_add(1, std::memory_order_relaxed);
        if (spins < max_spin_) {
            std::this_thread::yield();
            return;
        }

        std::unique_lock<std::mutex> lock(cv_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(1));
    }

    // 排空队列中剩余消息（停止时调用）
    void drain_remaining() {
        size_t drained = 0;
        while (true) {
            size_t r = read_idx_.load(std::memory_order_relaxed);
            if (r == write_idx_) break;

            LogicMessage& msg = queue_[r];
            TcpConnectionPtr conn;
            if (msg.conn_guard) {
                conn = std::static_pointer_cast<TcpConnection>(msg.conn_guard);
            }

            if (msg.type == LogicMessage::kMessage && msg_handler_) {
                std::string_view sv(msg.data.data(), msg.data.size());
                msg_handler_(conn, sv);
            } else if (msg.type == LogicMessage::kDisconnect && disc_handler_) {
                disc_handler_(conn);
            } else if (msg.type == LogicMessage::kTask && msg.task) {
                msg.task();
            }

            msg = LogicMessage();
            read_idx_.store((r + 1) % queue_capacity_, std::memory_order_release);
            ++drained;
        }
        if (drained > 0) {
            CHWELL_LOG_INFO("LogicThread drained " << drained << " remaining messages on stop");
        }
    }

    std::atomic<bool> running_;
    std::thread thread_;

    size_t queue_capacity_;
    LogicMessage* queue_;
    size_t write_idx_;
    std::atomic<size_t> read_idx_;

    std::mutex post_mutex_;
    std::mutex cv_mutex_;
    std::condition_variable cv_;

    MessageHandler msg_handler_;
    DisconnectHandler disc_handler_;
    StallHandler stall_handler_;  // 🆕

    size_t max_batch_;
    std::atomic<uint32_t> spin_count_;
    uint32_t max_spin_;

    // 🆕 阻塞监控
    int64_t slow_threshold_ms_;
    int64_t frame_threshold_ms_;
    std::atomic<uint64_t> slow_count_;
    std::atomic<uint64_t> blocked_count_;
    std::atomic<uint64_t> total_processed_;
};

} // namespace net
} // namespace chwell

