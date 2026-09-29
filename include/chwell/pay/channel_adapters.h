#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "chwell/pay/payment_gateway.h"

namespace chwell {
namespace pay {

// ========== 支付渠道适配骨架 ==========
//
// 提供微信 / 支付宝风格的 PaymentGateway 实现骨架：
// - 保留真实渠道的交互形态（prepay 参数、异步回调、验签）
// - 签名/网络用可注入的函数对象替换，单测不触网
// - 生产环境把 signer / http_post 换成真实 SDK 即可
//
// 本文件不做任何真实渠道对接，只定义扩展点与内存行为。

// 可注入签名器：输入待签文本，输出签名串
using SignFn = std::function<std::string(const std::string& payload)>;
// 验签器：payload + sign
using VerifyFn = std::function<bool(const std::string& payload, const std::string& sign)>;
// 下单 HTTP：返回渠道应答原文（骨架里可直接返回 mock JSON）
using HttpPostFn = std::function<bool(const std::string& url,
                                      const std::string& body,
                                      std::string& response)>;

struct ChannelConfig {
    std::string channel_name;
    std::string mch_id;       // 商户号
    std::string app_id;
    std::string api_key;      // 仅骨架持有；真实场景走密钥管理
    std::string notify_url;   // 回调地址
};

// ---- 微信风格 ----
class WechatPayAdapter : public PaymentGateway {
public:
    WechatPayAdapter(ChannelConfig cfg,
                     SignFn signer,
                     VerifyFn verifier,
                     HttpPostFn http_post = nullptr)
        : cfg_(std::move(cfg))
        , signer_(std::move(signer))
        , verifier_(std::move(verifier))
        , http_post_(std::move(http_post)) {
        if (cfg_.channel_name.empty()) cfg_.channel_name = "wechat";
    }

    PaymentOrder create_order(const std::string& user_id,
                              const std::string& product_id,
                              std::int64_t amount_cents,
                              const std::string& currency,
                              std::int64_t now) override {
        // 本地先落单
        PaymentOrder o = local_.create_order(user_id, product_id, amount_cents, currency, now);
        if (o.order_id.empty()) return o;

        // 组装统一下单参数（骨架）
        std::string payload = build_unified_order_payload(o);
        std::string sign = signer_ ? signer_(payload) : std::string();
        prepay_params_[o.order_id] = "{\"prepay_id\":\"wx_prepay_" + o.order_id
                                   + "\",\"sign\":\"" + sign + "\"}";
        return o;
    }

    bool handle_callback(const PaymentCallback& cb, std::string& message) override {
        if (!verifier_) {
            message = "no verifier";
            return false;
        }
        std::string payload = build_notify_payload(cb);
        if (!verifier_(payload, cb.sign)) {
            message = "sign verify failed";
            return false;
        }
        return local_.handle_callback(cb, message);
    }

    bool query_order(const std::string& order_id, PaymentOrder& out) override {
        return local_.query_order(order_id, out);
    }

    // 取预支付参数（给客户端拉起收银台）
    bool prepay_params(const std::string& order_id, std::string& out) const {
        auto it = prepay_params_.find(order_id);
        if (it == prepay_params_.end()) return false;
        out = it->second;
        return true;
    }

    const ChannelConfig& config() const { return cfg_; }

private:
    std::string build_unified_order_payload(const PaymentOrder& o) const {
        return "appid=" + cfg_.app_id
             + "&mch_id=" + cfg_.mch_id
             + "&out_trade_no=" + o.order_id
             + "&total_fee=" + std::to_string(o.amount_cents)
             + "&notify_url=" + cfg_.notify_url;
    }

    std::string build_notify_payload(const PaymentCallback& cb) const {
        return "out_trade_no=" + cb.order_id
             + "&transaction_id=" + cb.transaction_id
             + "&total_fee=" + std::to_string(cb.amount_cents);
    }

    ChannelConfig cfg_;
    SignFn signer_;
    VerifyFn verifier_;
    HttpPostFn http_post_;
    MemoryPaymentGateway local_{"wechat"};
    std::map<std::string, std::string> prepay_params_;
};

// ---- 支付宝风格 ----
class AlipayAdapter : public PaymentGateway {
public:
    AlipayAdapter(ChannelConfig cfg, SignFn signer, VerifyFn verifier)
        : cfg_(std::move(cfg))
        , signer_(std::move(signer))
        , verifier_(std::move(verifier)) {
        if (cfg_.channel_name.empty()) cfg_.channel_name = "alipay";
    }

    PaymentOrder create_order(const std::string& user_id,
                              const std::string& product_id,
                              std::int64_t amount_cents,
                              const std::string& currency,
                              std::int64_t now) override {
        PaymentOrder o = local_.create_order(user_id, product_id, amount_cents, currency, now);
        if (o.order_id.empty()) return o;
        std::string payload = build_trade_payload(o);
        std::string sign = signer_ ? signer_(payload) : std::string();
        // 支付宝常见形态：带签名的跳转串
        trade_form_[o.order_id] = "alipay_gateway?out_trade_no=" + o.order_id + "&sign=" + sign;
        return o;
    }

    bool handle_callback(const PaymentCallback& cb, std::string& message) override {
        if (!verifier_) {
            message = "no verifier";
            return false;
        }
        std::string payload = "out_trade_no=" + cb.order_id
                            + "&trade_no=" + cb.transaction_id
                            + "&total_amount=" + std::to_string(cb.amount_cents);
        if (!verifier_(payload, cb.sign)) {
            message = "sign verify failed";
            return false;
        }
        return local_.handle_callback(cb, message);
    }

    bool query_order(const std::string& order_id, PaymentOrder& out) override {
        return local_.query_order(order_id, out);
    }

    bool trade_form(const std::string& order_id, std::string& out) const {
        auto it = trade_form_.find(order_id);
        if (it == trade_form_.end()) return false;
        out = it->second;
        return true;
    }

    const ChannelConfig& config() const { return cfg_; }

private:
    std::string build_trade_payload(const PaymentOrder& o) const {
        return "app_id=" + cfg_.app_id
             + "&out_trade_no=" + o.order_id
             + "&total_amount=" + std::to_string(o.amount_cents)
             + "&notify_url=" + cfg_.notify_url;
    }

    ChannelConfig cfg_;
    SignFn signer_;
    VerifyFn verifier_;
    MemoryPaymentGateway local_{"alipay"};
    std::map<std::string, std::string> trade_form_;
};

}  // namespace pay
}  // namespace chwell
