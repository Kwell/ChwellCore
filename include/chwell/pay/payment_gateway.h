#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace chwell {
namespace pay {

// ========== 支付 SDK 抽象 ==========
//
// 与具体渠道解耦：业务只依赖 PaymentGateway 接口。
// - create_order / handle_callback / query_order
// - 内置 MemoryPaymentGateway 用于单测与开发环境
// - 回调验签由渠道适配器实现；此处统一状态机
//
// 订单状态：Created → Paid | Closed | Refunded

enum class OrderStatus : std::uint8_t {
    Created = 0,
    Paid = 1,
    Closed = 2,
    Refunded = 3,
};

inline const char* order_status_name(OrderStatus s) {
    switch (s) {
        case OrderStatus::Created: return "created";
        case OrderStatus::Paid: return "paid";
        case OrderStatus::Closed: return "closed";
        case OrderStatus::Refunded: return "refunded";
    }
    return "unknown";
}

struct PaymentOrder {
    std::string order_id;
    std::string user_id;
    std::string product_id;
    std::int64_t amount_cents = 0;  // 以分为单位，避免浮点
    std::string currency = "CNY";
    OrderStatus status = OrderStatus::Created;
    std::int64_t create_time = 0;
    std::int64_t pay_time = 0;
};

// 渠道回调载荷
struct PaymentCallback {
    std::string order_id;
    std::string channel;     // 如 "mock" / "wechat" / "alipay"
    std::string transaction_id;
    std::int64_t amount_cents = 0;
    std::string sign;        // 渠道签名，适配器负责校验
    bool success = true;
};

class PaymentGateway {
public:
    virtual ~PaymentGateway() = default;

    // 创建预支付订单；失败返回空 order_id
    virtual PaymentOrder create_order(const std::string& user_id,
                                      const std::string& product_id,
                                      std::int64_t amount_cents,
                                      const std::string& currency,
                                      std::int64_t now) = 0;

    // 处理渠道回调（应验签）；返回是否接受
    virtual bool handle_callback(const PaymentCallback& cb, std::string& message) = 0;

    virtual bool query_order(const std::string& order_id, PaymentOrder& out) = 0;
};

// 内存渠道：开发/测试用，不过真实网络
class MemoryPaymentGateway : public PaymentGateway {
public:
    explicit MemoryPaymentGateway(std::string channel = "mock") : channel_(std::move(channel)) {}

    PaymentOrder create_order(const std::string& user_id,
                              const std::string& product_id,
                              std::int64_t amount_cents,
                              const std::string& currency,
                              std::int64_t now) override {
        PaymentOrder o;
        if (user_id.empty() || product_id.empty() || amount_cents <= 0) {
            return o;  // order_id 空表示失败
        }
        std::lock_guard<std::mutex> lock(mu_);
        o.order_id = channel_ + "_" + std::to_string(++next_id_);
        o.user_id = user_id;
        o.product_id = product_id;
        o.amount_cents = amount_cents;
        o.currency = currency.empty() ? "CNY" : currency;
        o.status = OrderStatus::Created;
        o.create_time = now;
        orders_[o.order_id] = o;
        return o;
    }

    bool handle_callback(const PaymentCallback& cb, std::string& message) override {
        if (cb.order_id.empty()) {
            message = "missing order_id";
            return false;
        }
        // 简化验签：要求 sign == "ok_<order_id>"（真实渠道换成 RSA/HMAC）
        if (cb.sign != "ok_" + cb.order_id) {
            message = "bad sign";
            return false;
        }
        std::lock_guard<std::mutex> lock(mu_);
        auto it = orders_.find(cb.order_id);
        if (it == orders_.end()) {
            message = "order not found";
            return false;
        }
        PaymentOrder& o = it->second;
        if (o.status == OrderStatus::Paid) {
            // 幂等：重复回调不报错
            message = "already paid";
            return true;
        }
        if (o.status != OrderStatus::Created) {
            message = "invalid state";
            return false;
        }
        if (cb.amount_cents != o.amount_cents) {
            message = "amount mismatch";
            return false;
        }
        if (!cb.success) {
            o.status = OrderStatus::Closed;
            message = "closed by channel";
            return true;
        }
        o.status = OrderStatus::Paid;
        o.pay_time = o.create_time;  // 内存渠道无独立支付时间戳
        message = "ok";
        return true;
    }

    bool query_order(const std::string& order_id, PaymentOrder& out) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = orders_.find(order_id);
        if (it == orders_.end()) return false;
        out = it->second;
        return true;
    }

    // 渠道适配器在完成自有验签后走此入口（跳过 Memory 渠道的固定 ok_ 签名校验）
    bool settle_verified(const PaymentCallback& cb, std::string& message) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = orders_.find(cb.order_id);
        if (it == orders_.end()) {
            message = "order not found";
            return false;
        }
        PaymentOrder& o = it->second;
        if (o.status == OrderStatus::Paid) {
            message = "already paid";
            return true;
        }
        if (o.status != OrderStatus::Created) {
            message = "invalid state";
            return false;
        }
        if (cb.amount_cents != o.amount_cents) {
            message = "amount mismatch";
            return false;
        }
        if (!cb.success) {
            o.status = OrderStatus::Closed;
            message = "closed by channel";
            return true;
        }
        o.status = OrderStatus::Paid;
        o.pay_time = o.create_time;
        message = "ok";
        return true;
    }

    // 主动退款（测试/管理台）
    bool refund(const std::string& order_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = orders_.find(order_id);
        if (it == orders_.end() || it->second.status != OrderStatus::Paid) return false;
        it->second.status = OrderStatus::Refunded;
        return true;
    }

    std::size_t order_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return orders_.size();
    }

private:
    std::string channel_;
    std::uint64_t next_id_ = 0;
    mutable std::mutex mu_;
    std::unordered_map<std::string, PaymentOrder> orders_;
};

}  // namespace pay
}  // namespace chwell
