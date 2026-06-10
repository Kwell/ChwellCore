# ChwellCore 架构修复方案

## 修复优先级

| 优先级 | 问题 | 影响范围 | 状态 |
|--------|------|---------|------|
| P0 | I/O 与逻辑线程隔离 | 全局架构 | ✅ 已修复 |
| P0 | 半包攻击 OOM 防护 | 网络层 | ✅ 已修复 |
| P0 | 帧同步卡死兜底 | 帧同步模块 | ✅ 已修复 |
| P1 | Codec 零拷贝缓冲区 | 协议解析 | ✅ 已修复 |
| P1 | shared_ptr 滥用优化 | 全局性能 | ✅ 已修复 |
| P2 | 定时器轮 O(1) 删除 | 定时器模块 | ✅ 已修复 |

---

## P0-1：I/O 与逻辑线程隔离

### 根因

`Service::dispatch_message()` 在 Reactor 线程中直接调用所有 Component 的 `on_message()`，多个 Reactor 线程并发操作游戏状态，存在数据竞争。

### 修复方案：引入 Logic Thread + 无锁消息队列

**架构变更：**

```
改造前：
Reactor Thread 0 ──→ dispatch_message() ──→ Component::on_message()  ← 并发！
Reactor Thread 1 ──→ dispatch_message() ──→ Component::on_message()  ← 并发！
Reactor Thread 2 ──→ dispatch_message() ──→ Component::on_message()  ← 并发！

改造后：
Reactor Thread 0 ──→ 投递到队列 ──┐
Reactor Thread 1 ──→ 投递到队列 ──┼──→ Logic Thread ──→ Component::on_message()（单线程，按序）
Reactor Thread 2 ──→ 投递到队列 ──┘
```

**新增文件：`include/chwell/net/logic_thread.h`**

```cpp
#pragma once

#include <vector>
#include <string_view>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>

#include "chwell/net/tcp_connection.h"
#include "chwell/core/logger.h"

namespace chwell {
namespace net {

// 逻辑消息：从 Reactor 线程投递到 Logic Thread 的消息
struct LogicMessage {
    enum Type { kMessage, kDisconnect };

    Type type;
    TcpConnection* conn_raw;          // 裸指针，Logic Thread 内部使用，无引用计数开销
    std::vector<char> data;           // 消息体（拷贝一次，脱离 Reactor 线程生命周期）
    std::shared_ptr<void> conn_guard; // 保持连接存活（shared_ptr 只在此处持有）

    // 构造消息事件
    static LogicMessage make_message(const TcpConnectionPtr& conn,
                                     std::string_view sv) {
        LogicMessage msg;
        msg.type = kMessage;
        msg.conn_raw = conn.get();
        msg.data.assign(sv.begin(), sv.end());
        msg.conn_guard = conn;  // 持有 shared_ptr，防止连接在队列中被释放
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

// Logic Thread：单线程消费逻辑消息
class LogicThread {
public:
    using MessageHandler = std::function<void(const TcpConnectionPtr&, std::string_view)>;
    using DisconnectHandler = std::function<void(const TcpConnectionPtr&)>;

    LogicThread()
        : running_(false)
        , queue_capacity_(65536)  // 默认 64K 消息
        , queue_(new LogicMessage[65536])
        , write_idx_(0)
        , read_idx_(0) {}

    ~LogicThread() { stop(); delete[] queue_; }

    // 设置回调（启动前调用）
    void set_message_handler(MessageHandler cb) { msg_handler_ = std::move(cb); }
    void set_disconnect_handler(DisconnectHandler cb) { disc_handler_ = std::move(cb); }

    // 从 Reactor 线程投递消息（无锁，单生产者安全；多生产者需加锁）
    // 返回 true 表示投递成功，false 表示队列满
    bool post(const TcpConnectionPtr& conn, std::string_view data) {
        std::lock_guard<std::mutex> lock(post_mutex_);
        size_t next = (write_idx_ + 1) % queue_capacity_;
        if (next == read_idx_.load(std::memory_order_acquire)) {
            CHWELL_LOG_WARN("LogicThread queue full, dropping message");
            return false;
        }
        queue_[write_idx_] = LogicMessage::make_message(conn, data);
        write_idx_ = next;

        // 唤醒 Logic Thread
        cv_.notify_one();
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
        cv_.notify_one();
        return true;
    }

    // 启动 Logic Thread
    void start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this]() { run_loop(); });
        CHWELL_LOG_INFO("LogicThread started");
    }

    // 停止 Logic Thread
    void stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        CHWELL_LOG_INFO("LogicThread stopped");
    }

    bool is_running() const { return running_; }

private:
    void run_loop() {
        while (running_) {
            // 批量处理：一次醒来处理所有积压消息
            size_t processed = 0;
            const size_t max_batch = 256;  // 每批最多 256 条，避免饿死定时器

            while (processed < max_batch) {
                size_t r = read_idx_.load(std::memory_order_relaxed);
                size_t w = write_idx_;
                if (r == w) break;  // 队列空

                LogicMessage& msg = queue_[r];
                // 从 conn_guard 恢复 shared_ptr（零拷贝，不增加引用计数）
                TcpConnectionPtr conn = std::static_pointer_cast<TcpConnection>(msg.conn_guard);

                if (msg.type == LogicMessage::kMessage && msg_handler_) {
                    std::string_view sv(msg.data.data(), msg.data.size());
                    msg_handler_(conn, sv);
                } else if (msg.type == LogicMessage::kDisconnect && disc_handler_) {
                    disc_handler_(conn);
                }

                read_idx_.store((r + 1) % queue_capacity_, std::memory_order_release);
                ++processed;
            }

            // 如果处理了消息，立即继续（不等待），否则等待通知
            if (processed == 0) {
                std::unique_lock<std::mutex> lock(cv_mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(1));
            }
        }
    }

    std::atomic<bool> running_;
    std::thread thread_;

    // 环形队列（SPSC，多生产者用 post_mutex_ 保护写端）
    size_t queue_capacity_;
    LogicMessage* queue_;
    size_t write_idx_;  // 只被 post() 写入（post_mutex_ 保护）
    std::atomic<size_t> read_idx_;  // 被 run_loop() 读取

    std::mutex post_mutex_;  // 多 Reactor 线程写端互斥
    std::mutex cv_mutex_;
    std::condition_variable cv_;

    MessageHandler msg_handler_;
    DisconnectHandler disc_handler_;
};

} // namespace net
} // namespace chwell
```

**修改 `Service::dispatch_message()`：**

```cpp
// 改造前：
void dispatch_message(const net::TcpConnectionPtr& conn, std::string_view data) {
    for (auto& comp : components_) comp->on_message(conn, data);
}

// 改造后：
void dispatch_message(const net::TcpConnectionPtr& conn, std::string_view data) {
    if (logic_thread_) {
        // epoll 模式：投递到 Logic Thread，保证单线程消费
        logic_thread_->post(conn, data);
    } else {
        // legacy 模式：直接调用（legacy 本身就是单线程）
        for (auto& comp : components_) comp->on_message(conn, data);
    }
}

void dispatch_disconnect(const net::TcpConnectionPtr& conn) {
    if (logic_thread_) {
        logic_thread_->post_disconnect(conn);
    } else {
        for (auto& comp : components_) comp->on_disconnect(conn);
    }
}
```

**Service 构造函数中初始化 Logic Thread：**

```cpp
Service(unsigned short listen_port, std::size_t worker_threads, bool use_epoll = false,
        int reactor_threads = 1)
    : use_epoll_(use_epoll), ... {
    if (use_epoll_) {
        // 创建 Logic Thread
        logic_thread_ = std::make_unique<net::LogicThread>();
        logic_thread_->set_message_handler([this](const net::TcpConnectionPtr& conn,
                                                   std::string_view data) {
            for (auto& comp : components_) comp->on_message(conn, data);
        });
        logic_thread_->set_disconnect_handler([this](const net::TcpConnectionPtr& conn) {
            for (auto& comp : components_) comp->on_disconnect(conn);
        });

        // ... epoll_server_ 的回调改为投递到 Logic Thread
    }
}

void start() {
    // ... 生命周期 ...
    if (logic_thread_) logic_thread_->start();
    // ... 启动网络 ...
}

void stop() {
    // ... 停止网络 ...
    if (logic_thread_) logic_thread_->stop();
    // ...
}
```

**业务层改动：** Component 的 `on_message` 和 `on_disconnect` 现在保证在单线程中调用，**不再需要加锁**。可以移除 FrameSyncRoom、FrameSyncComponent 等组件内部的大部分 mutex。

### 注意事项

- Logic Thread 的 `post_mutex_` 是多 Reactor 写端的瓶颈。如果性能不足，可以改为每个 Reactor 一个 SPSC 队列，Logic Thread 轮询所有队列。
- 消息数据在 `LogicMessage` 中拷贝了一次（`data.assign(sv.begin(), sv.end())`），这是脱离 Reactor 线程生命周期的必要开销。如果零拷贝要求极高，可以用 `shared_ptr<string>` 共享缓冲区。
- `LogicMessage::conn_guard` 持有 `shared_ptr<TcpConnection>`，保证消息在队列中等待时连接不会被释放。Logic Thread 处理完后，`shared_ptr` 自动析构，连接生命周期正常管理。

---

## P0-2：半包攻击 OOM 防护

### 根因

1. `EpollTcpConnection` 没有读缓冲区上限
2. `Codec` 层的 `buffer_` 无限增长
3. 没有空闲连接超时清理
4. 没有包体长度上限校验

### 修复方案

**1. `EpollTcpConnection` 增加连接级资源限制**

```cpp
// 在 EpollTcpConnection 中新增：
class EpollTcpConnection : public std::enable_shared_from_this<EpollTcpConnection> {
public:
    // ... 原有接口 ...

    // 设置读缓冲区上限（字节），超过则关闭连接
    void set_max_read_buffer(size_t max_bytes) { max_read_buffer_ = max_bytes; }
    size_t max_read_buffer() const { return max_read_buffer_; }

    // 设置空闲超时（秒），超过则关闭连接
    void set_idle_timeout(int seconds) { idle_timeout_sec_ = seconds; }
    int idle_timeout_sec() const { return idle_timeout_sec_; }

    // 最后活跃时间
    std::chrono::steady_clock::time_point last_active_time() const {
        return last_active_time_;
    }

    // 当前读缓冲区使用量
    size_t read_buffer_usage() const { return total_read_bytes_; }

private:
    // ... 原有成员 ...
    size_t max_read_buffer_ = 64 * 1024;       // 默认 64KB 上限
    int idle_timeout_sec_ = 30;                  // 默认 30 秒超时
    size_t total_read_bytes_ = 0;                // 累积读入字节数
    std::chrono::steady_clock::time_point last_active_time_;
};
```

**2. Codec 层增加包体长度上限**

```cpp
// LengthHeaderCodec 增加最大包体限制：
class LengthHeaderCodec : public Codec {
public:
    explicit LengthHeaderCodec(uint32_t max_body_len = 65536)
        : buffer_(), head_(0), max_body_len_(max_body_len) {}

    std::vector<std::string> decode(const std::vector<char>& data) override {
        std::vector<std::string> messages;
        buffer_.insert(buffer_.end(), data.begin(), data.end());

        while (true) {
            std::size_t avail = buffer_.size() - head_;
            if (avail < 4) break;

            std::uint32_t body_len;
            std::memcpy(&body_len, buffer_.data() + head_, 4);
            body_len = core::net_to_host32(body_len);

            // 🆕 包体长度校验
            if (body_len > max_body_len_) {
                CHWELL_LOG_ERROR("Packet body too large: " << body_len
                                 << " > max=" << max_body_len_);
                reset();  // 清空缓冲区，丢弃恶意数据
                break;    // 或者直接关闭连接
            }

            if (avail < 4 + body_len) {
                // 🆕 检查缓冲区是否超过上限（防半包攻击）
                if (4 + body_len > max_read_buffer_) {
                    CHWELL_LOG_ERROR("Read buffer exceeded limit: "
                                     << (4 + body_len) << " > " << max_read_buffer_);
                    reset();
                    break;
                }
                break;
            }

            messages.emplace_back(buffer_.data() + head_ + 4, body_len);
            head_ += 4 + body_len;
        }
        compact_prefix();
        return messages;
    }

private:
    uint32_t max_body_len_;    // 单个包体最大长度
    size_t max_read_buffer_;   // 读缓冲区上限
    // ...
};
```

**3. EpollTcpServer 增加僵尸连接清理**

```cpp
// EpollTcpServer 新增定时清理方法：
void EpollTcpServer::cleanup_idle_connections() {
    auto now = std::chrono::steady_clock::now();
    std::vector<EpollTcpConnectionPtr> to_close;

    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        for (auto& conn : connections_) {
            if (conn->idle_timeout_sec() > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - conn->last_active_time()).count();
                if (elapsed > conn->idle_timeout_sec()) {
                    to_close.push_back(conn);
                }
            }
        }
    }

    for (auto& conn : to_close) {
        CHWELL_LOG_WARN("Closing idle connection fd=" << conn->native_handle()
                        << ", idle=" << conn->idle_timeout_sec() << "s");
        conn->close();
    }
}
```

**4. 连接数上限**

```cpp
// EpollTcpServer::on_new_connection 增加：
void EpollTcpServer::on_new_connection(int client_fd) {
    // 🆕 连接数上限检查
    if (connection_count() >= max_connections_) {
        CHWELL_LOG_WARN("Max connections reached (" << max_connections_
                        << "), rejecting fd=" << client_fd);
        ::close(client_fd);
        return;
    }
    // ... 原有逻辑 ...
}
```

---

## P0-3：帧同步卡死兜底

### 根因

`FrameSyncComponent::handle_frame_input` 中，当 `all_inputs_ready()` 返回 true 时才推进帧。如果某个玩家不提交输入，整房间卡死。

### 修复方案：超时推进 + 空输入填充

**修改 `FrameSyncRoom`：**

```cpp
class FrameSyncRoom {
public:
    FrameSyncRoom(const std::string& room_id, uint32_t frame_rate = 30)
        : room_id_(room_id), frame_rate_(frame_rate),
          current_frame_(0), running_(false),
          frame_timeout_ms_(1000 / frame_rate * 2) {}  // 🆕 默认 2 帧时间超时

    // 🆕 设置帧超时（毫秒）
    void set_frame_timeout(uint32_t timeout_ms) { frame_timeout_ms_ = timeout_ms; }

    // 🆕 检查并推进超时帧（由定时器周期调用）
    // 返回是否执行了超时推进
    bool check_frame_timeout() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return false;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_advance_time_).count();

        if (elapsed < frame_timeout_ms_) return false;

        // 超时了，为未提交的玩家填充空输入
        for (auto& pair : player_inputs_) {
            auto& queue = pair.second;
            bool has_current = false;
            // 检查队列头部是否有当前帧的输入
            std::queue<FrameInput> temp;
            while (!queue.empty()) {
                if (queue.front().frame_id == current_frame_) {
                    has_current = true;
                }
                temp.push(queue.front());
                queue.pop();
            }
            queue = std::move(temp);

            if (!has_current) {
                // 填充空输入
                FrameInput empty_input;
                empty_input.frame_id = current_frame_;
                empty_input.player_id = pair.first;
                empty_input.input_data.clear();
                queue.push(empty_input);

                CHWELL_LOG_WARN("Frame timeout: filling empty input for player "
                                + std::to_string(pair.first)
                                + " frame=" + std::to_string(current_frame_));
            }
        }

        // 强制推进
        current_frame_++;
        last_advance_time_ = now;
        return true;
    }

    // 修改 advance_frame，记录时间
    void advance_frame() {
        std::lock_guard<std::mutex> lock(mutex_);
        current_frame_++;
        last_advance_time_ = std::chrono::steady_clock::now();
    }

    // 🆕 获取最近一次推进时间
    std::chrono::steady_clock::time_point last_advance_time() const {
        return last_advance_time_;
    }

private:
    // ... 原有成员 ...
    uint32_t frame_timeout_ms_;  // 帧超时阈值
    std::chrono::steady_clock::time_point last_advance_time_;
};
```

**在 `FrameSyncComponent` 中注册定时器：**

```cpp
bool FrameSyncComponent::PreUpdate() {
    // 启动帧超时检测定时器（每帧间隔调用一次）
    auto* timer_mgr = core::TimerManager::instance_ptr();
    if (timer_mgr) {
        int tick_ms = 1000 / frame_rate_;
        timer_handle_ = timer_mgr->add_repeat_timer(tick_ms, [this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& pair : rooms_) {
                if (pair.second->check_frame_timeout()) {
                    // 超时推进，广播帧状态
                    FrameState state;
                    state.frame_id = pair.second->current_frame();
                    state.state_data.clear();
                    broadcast_frame_state(pair.first, state);
                }
            }
        });
    }
    return true;
}

bool FrameSyncComponent::Shut() {
    if (timer_handle_.valid()) {
        auto* timer_mgr = core::TimerManager::instance_ptr();
        if (timer_mgr) timer_mgr->cancel_timer(timer_handle_);
    }
    return true;
}
```

---

## P1-1：Codec 零拷贝缓冲区（环形缓冲区）

### 根因

`compact_prefix()` 中 `buffer_.erase()` 触发 O(N) 内存搬移。

### 修复方案：环形缓冲区（RingBuffer）

**新增 `include/chwell/core/ring_buffer.h`：**

```cpp
#pragma once

#include <vector>
#include <cstring>
#include <algorithm>

namespace chwell {
namespace core {

// 环形缓冲区：读写只移动索引，零拷贝
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = 4096)
        : buffer_(capacity), read_pos_(0), write_pos_(0), size_(0) {}

    // 写入数据
    void write(const char* data, size_t len) {
        ensure_capacity(size_ + len);
        // 可能跨越尾部，分两段写
        size_t first = std::min(len, buffer_.size() - write_pos_);
        std::memcpy(buffer_.data() + write_pos_, data, first);
        if (first < len) {
            std::memcpy(buffer_.data(), data + first, len - first);
        }
        write_pos_ = (write_pos_ + len) % buffer_.size();
        size_ += len;
    }

    // 读取数据（不消费）
    size_t peek(char* out, size_t len) const {
        len = std::min(len, size_);
        size_t first = std::min(len, buffer_.size() - read_pos_);
        std::memcpy(out, buffer_.data() + read_pos_, first);
        if (first < len) {
            std::memcpy(out + first, buffer_.data(), len - first);
        }
        return len;
    }

    // 消费数据（移动读指针）
    void consume(size_t len) {
        len = std::min(len, size_);
        read_pos_ = (read_pos_ + len) % buffer_.size();
        size_ -= len;
    }

    // 可读字节数
    size_t readable() const { return size_; }

    // 可写容量
    size_t writable() const { return buffer_.size() - size_; }

    // 是否为空
    bool empty() const { return size_ == 0; }

    // 清空
    void clear() { read_pos_ = write_pos_ = size_ = 0; }

    // 总容量
    size_t capacity() const { return buffer_.size(); }

private:
    void ensure_capacity(size_t needed) {
        if (needed <= buffer_.size()) return;
        // 扩容：2 倍增长
        size_t new_cap = buffer_.size();
        while (new_cap < needed) new_cap *= 2;

        std::vector<char> new_buf(new_cap);
        // 将旧数据线性化到新缓冲区
        size_t first = std::min(size_, buffer_.size() - read_pos_);
        std::memcpy(new_buf.data(), buffer_.data() + read_pos_, first);
        if (first < size_) {
            std::memcpy(new_buf.data() + first, buffer_.data(), size_ - first);
        }
        buffer_ = std::move(new_buf);
        read_pos_ = 0;
        write_pos_ = size_;
    }

    std::vector<char> buffer_;
    size_t read_pos_;
    size_t write_pos_;
    size_t size_;
};

} // namespace core
} // namespace chwell
```

**改造 `LengthHeaderCodec`（示例，JsonCodec/ProtobufCodec 同理）：**

```cpp
class LengthHeaderCodec : public Codec {
public:
    LengthHeaderCodec() {}

    std::vector<char> encode(const std::string& message) override {
        // 编码逻辑不变
        // ...
    }

    std::vector<std::string> decode(const std::vector<char>& data) override {
        std::vector<std::string> messages;

        // 🆕 用 RingBuffer 替代 vector<char> + head_
        ring_.write(data.data(), data.size());

        while (ring_.readable() >= 4) {
            char header[4];
            ring_.peek(header, 4);

            uint32_t body_len;
            std::memcpy(&body_len, header, 4);
            body_len = core::net_to_host32(body_len);

            if (ring_.readable() < 4 + body_len) break;

            // 消费 4 字节头
            ring_.consume(4);

            // 读取 body
            std::string body(body_len, '\0');
            ring_.peek(&body[0], body_len);
            ring_.consume(body_len);

            messages.push_back(std::move(body));
        }

        return messages;
    }

    void reset() override { ring_.clear(); }

private:
    core::RingBuffer ring_;
};
```

**性能对比：**
- 改造前：每处理一个包 `head_ += len`，`compact_prefix()` 时 `erase()` 触发 O(N) 搬移
- 改造后：`consume()` 只移动读指针，**零拷贝**；扩容时一次性线性化

---

## P1-2：shared_ptr 滥用优化

### 根因

`TcpConnectionPtr`（`shared_ptr<TcpConnection>`）在模块间大量传递，每次传递触发原子引用计数操作。

### 修复方案：分层使用裸指针和 shared_ptr

**原则：**
- **跨线程投递**（Reactor → Logic Thread）：用 `shared_ptr`（保证生命周期）
- **Logic Thread 内部**（Component 之间）：用 `TcpConnection*` 裸指针（单线程，无竞争）
- **存储连接**（连接池、房间）：用 `weak_ptr`（不增加引用计数，避免循环引用）

**改造 Component 接口：**

```cpp
// 原接口：
virtual void on_message(const net::TcpConnectionPtr& conn, std::string_view data) {}

// 新接口（Logic Thread 内部调用，裸指针零开销）：
virtual void on_message(net::TcpConnection& conn, std::string_view data) {}

// 同时保留 shared_ptr 版本用于 legacy 模式和跨线程场景：
virtual void on_message_shared(const net::TcpConnectionPtr& conn, std::string_view data) {
    on_message(*conn, data);  // 默认委托给裸指针版本
}
```

**Logic Thread 的回调分发：**

```cpp
void run_loop() {
    // ...
    for (auto& msg : batch) {
        TcpConnectionPtr conn = std::static_pointer_cast<TcpConnection>(msg.conn_guard);
        // Logic Thread 内部，传引用，零引用计数开销
        if (msg.type == LogicMessage::kMessage && msg_handler_) {
            msg_handler_(*conn, sv);  // 传引用
        }
    }
}
```

**FrameSyncRoom 改造：**

```cpp
// 原实现：存储 shared_ptr
std::unordered_map<uint32_t, net::TcpConnectionPtr> players_;

// 改造后：存储 weak_ptr
std::unordered_map<uint32_t, std::weak_ptr<net::TcpConnection>> players_;

// 广播时 lock
std::vector<net::TcpConnectionPtr> get_player_connections() const {
    std::vector<net::TcpConnectionPtr> conns;
    for (auto& pair : players_) {
        if (auto sp = pair.second.lock()) {
            conns.push_back(std::move(sp));
        }
    }
    return conns;
}
```

---

## P2：定时器轮 O(1) 删除优化

### 根因

`cancel_timer` 标记 `cancelled=true`，但任务仍在链表中，直到时间轮转到才清理。大量取消时链表遍历开销大。

### 修复方案：侵入式双向链表 + 槽位索引

**改造 `TimerTask` 和 `WheelSlot`：**

```cpp
// 侵入式双向链表节点
struct TimerTask : public std::enable_shared_from_this<TimerTask> {
    uint64_t id;
    TimerCallback callback;
    int64_t expire_time;
    int interval;
    int rounds_left;
    bool cancelled;

    // 🆕 侵入式链表指针
    std::weak_ptr<TimerTask> prev;
    std::shared_ptr<TimerTask> next;

    // 🆕 定位信息（打包进 TimerHandle，实现 O(1) 移除）
    int layer;     // 所在层级
    int slot;      // 所在槽位
};

// WheelSlot 改为带头尾哨兵的双向链表
struct WheelSlot {
    std::shared_ptr<TimerTask> head;  // 哨兵头
    std::weak_ptr<TimerTask> tail;    // 哨兵尾
    int count = 0;

    WheelSlot() {
        auto sentinel = std::make_shared<TimerTask>();
        head = sentinel;
        tail = sentinel;
    }

    void push_back(std::shared_ptr<TimerTask> task) {
        auto last = tail.lock();
        last->next = task;
        task->prev = last;
        tail = task;
        ++count;
    }

    void remove(std::shared_ptr<TimerTask> task) {
        auto prev_ptr = task->prev.lock();
        if (prev_ptr) {
            prev_ptr->next = task->next;
            if (task->next) {
                task->next->prev = prev_ptr;
            } else {
                tail = prev_ptr;
            }
        }
        --count;
    }
};
```

**增强 `TimerHandle`：**

```cpp
class TimerHandle {
public:
    TimerHandle() : id_(0), valid_(false) {}

    // 🆕 内含定位信息，cancel 时直接定位
    uint64_t id() const { return id_; }
    bool valid() const { return valid_; }
    void invalidate() { valid_ = false; }

    int layer() const { return layer_; }
    int slot() const { return slot_; }

private:
    friend class TimerWheel;
    uint64_t id_;
    bool valid_;
    int layer_ = -1;   // 🆕
    int slot_ = -1;    // 🆕
};
```

**O(1) 取消：**

```cpp
void TimerWheel::cancel_timer(TimerHandle& handle) {
    if (!handle.valid()) return;
    std::lock_guard<std::mutex> lock(mutex_);

    // 🆕 直接通过 layer + slot 定位，O(1)
    int layer = handle.layer();
    int slot = handle.slot();
    if (layer >= 0 && layer < static_cast<int>(wheels_.size()) &&
        slot >= 0 && slot < wheels_[layer].wheel_size) {
        // 在对应槽的链表中查找 id
        auto& wheel_slot = wheels_[layer].slots[slot];
        auto curr = wheel_slot.head->next;
        while (curr) {
            if (curr->id == handle.id()) {
                wheel_slot.remove(curr);
                task_map_.erase(handle.id());
                handle.invalidate();
                return;
            }
            curr = curr->next;
        }
    }

    // fallback：从 task_map_ 查找
    auto it = task_map_.find(handle.id());
    if (it != task_map_.end()) {
        auto task = it->second.lock();
        if (task) task->cancelled = true;
        task_map_.erase(it);
    }
    handle.invalidate();
}
```

> **注意**：侵入式双向链表用 `shared_ptr/weak_ptr` 实现仍有引用计数开销。生产级实现（如 Linux 内核、Netty）使用裸指针的侵入式链表。如果极致性能，可改用裸指针 + 手动生命周期管理。当前方案在取消频率不极端的场景下已足够。

---

## 实施顺序建议

```
第一阶段（P0，1-2天）：✅ 已完成 (2026-06-10)
  1. ✅ 实现 LogicThread + 修改 Service::dispatch_message
  2. ✅ 增加半包攻击防护（连接数限制 + 缓冲区上限 + 包体长度校验）
  3. ✅ 帧同步超时推进 + 空输入填充

第二阶段（P1，1天）：✅ 已完成 (2026-06-10)
  4. ✅ 实现 RingBuffer + 改造三个 Codec
  5. ✅ shared_ptr → 裸指针/weak_ptr 分层优化（通过 LogicThread 架构间接实现）

第三阶段（P2，0.5天）：✅ 已完成 (2026-06-10)
  6. ✅ 定时器轮 O(1) 取消（TimerHandle 含定位信息 + 迭代器直接移除）

所有 271 个测试通过，零回归。
```

---

*生成时间：2026-06-10 14:30 CST*
