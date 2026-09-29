#pragma once

// 分布式追踪上下文：跨 RPC / 跨线程传递 trace_id
//
// 用法：
//   trace::ScopedSpan span("rpc.call");        // 作用域内自动记录耗时
//   auto tid = trace::TraceContext::current_trace_id();
//   // RPC 序列化时携带 tid，服务端 trace::TraceContext::restore(tid, sid)
//
// 约定：
// - trace_id: 全链路唯一（128bit 以 2 个 uint64 表示，简化为单 uint64 + 随机）
// - span_id: 当前环节 ID
// - 逻辑线程切换时用 restore() 显式恢复（LogicMessage 里带上即可）

#include <cstdint>
#include <string>
#include <chrono>
#include <atomic>
#include <functional>

#include "chwell/core/logger.h"

namespace chwell {
namespace trace {

struct TraceContext {
    std::uint64_t trace_id = 0;
    std::uint64_t span_id = 0;
    std::uint64_t parent_span_id = 0;

    static std::uint64_t new_id() {
        static std::atomic<std::uint64_t> counter{1};
        // 简化：进程内自增 + 时间熵混合，足够日志关联；跨进程全局唯一需外部 ID 发号
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        return (static_cast<std::uint64_t>(now) << 16) ^ counter.fetch_add(1, std::memory_order_relaxed);
    }

    static TraceContext& current() {
        static thread_local TraceContext ctx;
        return ctx;
    }

    // 无追踪时自动生成根 trace
    static std::uint64_t current_or_create_trace_id() {
        auto& c = current();
        if (c.trace_id == 0) {
            c.trace_id = new_id();
            c.span_id = new_id();
        }
        return c.trace_id;
    }

    static void restore(std::uint64_t trace_id, std::uint64_t span_id, std::uint64_t parent = 0) {
        auto& c = current();
        c.trace_id = trace_id;
        c.span_id = span_id ? span_id : new_id();
        c.parent_span_id = parent;
    }

    static void clear() {
        auto& c = current();
        c.trace_id = 0;
        c.span_id = 0;
        c.parent_span_id = 0;
    }
};

// 作用域 span：构造新 span，析构时记录耗时（可挂接指标）
class ScopedSpan {
public:
    explicit ScopedSpan(const char* name)
        : name_(name), start_(std::chrono::steady_clock::now()) {
        auto& c = TraceContext::current();
        if (c.trace_id == 0) {
            c.trace_id = TraceContext::new_id();
        }
        parent_span_ = c.span_id;
        c.parent_span_id = parent_span_;
        c.span_id = TraceContext::new_id();
    }

    ~ScopedSpan() {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_).count();
        auto& c = TraceContext::current();
        CHWELL_LOG_DEBUG(std::string("span ") + name_ +
                         " trace=" + std::to_string(c.trace_id) +
                         " span=" + std::to_string(c.span_id) +
                         " ms=" + std::to_string(ms));
        c.span_id = parent_span_;
        if (on_finish_) on_finish_(name_, ms);
    }

    void set_finish_callback(std::function<void(const char*, int64_t)> cb) {
        on_finish_ = std::move(cb);
    }

private:
    const char* name_;
    std::chrono::steady_clock::time_point start_;
    std::uint64_t parent_span_ = 0;
    std::function<void(const char*, int64_t)> on_finish_;
};

} // namespace trace
} // namespace chwell
