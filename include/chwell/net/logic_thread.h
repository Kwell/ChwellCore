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
 */
struct LogicMessage {
    enum Type : uint8_t { kMessage = 0, kDisconnect = 1 };

    Type type;
    TcpConnection* conn_raw;          // 裸指针，Logic Thread 内部用
    std::vector<char> data;           // 消息体副本
    std::shared_ptr<void> conn_guard; // 持有 shared_ptr，防止连接在队列中被释放

    LogicMessage() : type(kMessage), conn_raw(nullptr) {}

    // 构造消息事件
    static LogicMessage make_message(const TcpConnectionPtr& conn,
                                     std::string_view sv) {
        LogicMessage msg;
        msg.type = kMessage;
        msg.conn_raw = conn.get();
        msg.data.assign(sv.begin(), sv.end());
        msg.conn_guard = conn;  // 持有 shared_ptr
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
};

/**
 * @brief Logic Thread：单线程消费逻辑消息
 *
 * 架构角色：
 * - Reactor 线程只负责 I/O 和解包
 * - 解完包后将消息投递到 LogicThread 的环形队列
 * - LogicThread 单线程按序消费，保证业务逻辑线程安全
 *
 * 性能特征：
 * - 环形队列，无动态内存分配
 * - 批量处理，每批最多 max_batch_ 条消息
 * - 空闲时 condition_variable 等待，不消耗 CPU
 */
class LogicThread {
public:
    using MessageHandler = std::function<void(const TcpConnectionPtr&, std::string_view)>;
    using DisconnectHandler = std::function<void(const TcpConnectionPtr&)>;

    explicit LogicThread(size_t queue_capacity = 65536)
        : running_(false)
        , queue_capacity_(queue_capacity)
        , queue_(new LogicMessage[queue_capacity])
        , write_idx_(0)
        , read_idx_(0)
        , max_batch_(256)
        , spin_count_(0)
        , max_spin_(100) {}

    ~LogicThread() {
        stop();
        delete[] queue_;
    }

    // 禁止拷贝
    LogicThread(const LogicThread&) = delete;
    LogicThread& operator=(const LogicThread&) = delete;

    // 设置回调（启动前调用）
    void set_message_handler(MessageHandler cb) { msg_handler_ = std::move(cb); }
    void set_disconnect_handler(DisconnectHandler cb) { disc_handler_ = std::move(cb); }

    // 设置批量处理上限
    void set_max_batch(size_t n) { max_batch_ = n; }

    /**
     * @brief 从 Reactor 线程投递消息
     * @return true 投递成功，false 队列满
     */
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

        // 唤醒 Logic Thread
        notify();
        return true;
    }

    /**
     * @brief 投递断连事件
     */
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

    // 启动 Logic Thread
    void start() {
        if (running_.exchange(true)) return;

        thread_ = std::thread([this]() {
            CHWELL_LOG_INFO("LogicThread started");
            run_loop();
            CHWELL_LOG_INFO("LogicThread stopped");
        });
    }

    // 停止 Logic Thread
    void stop() {
        if (!running_.exchange(false)) return;

        notify();
        if (thread_.joinable()) thread_.join();
    }

    bool is_running() const { return running_; }

    // 队列中等待处理的消息数（近似值，多线程下不精确）
    size_t pending_count() const {
        size_t w = write_idx_;
        size_t r = read_idx_.load(std::memory_order_relaxed);
        return (w >= r) ? (w - r) : (queue_capacity_ - r + w);
    }

private:
    void notify() {
        // 先尝试轻量级唤醒（spin），减少 condition_variable 开销
        spin_count_.store(0, std::memory_order_release);
        cv_.notify_one();
    }

    void run_loop() {
        while (running_) {
            // 批量处理：一次醒来处理所有积压消息
            size_t processed = 0;

            while (processed < max_batch_) {
                size_t r = read_idx_.load(std::memory_order_relaxed);
                if (r == write_idx_) break;  // 队列空

                LogicMessage& msg = queue_[r];

                // 从 conn_guard 恢复 shared_ptr
                TcpConnectionPtr conn = std::static_pointer_cast<TcpConnection>(msg.conn_guard);

                if (msg.type == LogicMessage::kMessage && msg_handler_) {
                    std::string_view sv(msg.data.data(), msg.data.size());
                    msg_handler_(conn, sv);
                } else if (msg.type == LogicMessage::kDisconnect && disc_handler_) {
                    disc_handler_(conn);
                }

                // 清空消息，释放 shared_ptr
                msg = LogicMessage();

                read_idx_.store((r + 1) % queue_capacity_, std::memory_order_release);
                ++processed;
            }

            if (processed > 0) {
                // 有消息处理，重置 spin 计数
                spin_count_.store(0, std::memory_order_relaxed);
                // 不等待，立即继续检查（可能有更多消息积压）
                continue;
            }

            // 没有消息，进入等待
            wait_for_messages();
        }

        // 停止前处理剩余消息
        drain();
    }

    void wait_for_messages() {
        // 短暂 spin-wait，减少 condition_variable 开销
        uint32_t spins = spin_count_.fetch_add(1, std::memory_order_relaxed);
        if (spins < max_spin_) {
            // spin 阶段：让出 CPU 但不睡眠
            std::this_thread::yield();
            return;
        }

        // spin 超过阈值，进入 condition_variable 等待
        std::unique_lock<std::mutex> lock(cv_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(1));
    }

    // 排空队列中剩余消息
    void drain() {
        while (true) {
            size_t r = read_idx_.load(std::memory_order_relaxed);
            if (r == write_idx_) break;

            LogicMessage& msg = queue_[r];
            TcpConnectionPtr conn = std::static_pointer_cast<TcpConnection>(msg.conn_guard);

            if (msg.type == LogicMessage::kMessage && msg_handler_) {
                std::string_view sv(msg.data.data(), msg.data.size());
                msg_handler_(conn, sv);
            } else if (msg.type == LogicMessage::kDisconnect && disc_handler_) {
                disc_handler_(conn);
            }

            msg = LogicMessage();
            read_idx_.store((r + 1) % queue_capacity_, std::memory_order_release);
        }
    }

    std::atomic<bool> running_;
    std::thread thread_;

    // 环形队列
    size_t queue_capacity_;
    LogicMessage* queue_;
    size_t write_idx_;                       // 只被 post() 写入（post_mutex_ 保护）
    std::atomic<size_t> read_idx_;           // 被 run_loop() 读取

    std::mutex post_mutex_;                  // 多 Reactor 线程写端互斥
    std::mutex cv_mutex_;
    std::condition_variable cv_;

    MessageHandler msg_handler_;
    DisconnectHandler disc_handler_;

    size_t max_batch_;                       // 每批最大处理数
    std::atomic<uint32_t> spin_count_;       // spin 计数
    uint32_t max_spin_;                      // spin 阈值
};

} // namespace net
} // namespace chwell
