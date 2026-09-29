#include <gtest/gtest.h>

#include <string>

#include "chwell/http/http_request.h"
#include "chwell/http/http_response.h"
#include "chwell/ops/gm_admin_api.h"
#include "chwell/ops/gm_console.h"
#include "chwell/pay/channel_adapters.h"

using namespace chwell;

// ==================== GmAdminApi ====================

namespace {

http::HttpRequest make_req(const std::string& method, const std::string& path,
                           const std::string& body = std::string()) {
    http::HttpRequest r;
    r.method = method;
    r.path = path;
    r.body = body;
    return r;
}

}  // namespace

TEST(GmAdminApiTest, Health) {
    ops::GmConsole console;
    ops::GmAdminApi api(&console);
    http::HttpRequest req = make_req("GET", "/gm/health");
    http::HttpResponse resp;
    api.handle(req, resp);
    EXPECT_EQ(200, resp.status_code);
    EXPECT_NE(std::string::npos, resp.body.find("\"ok\":true"));
}

TEST(GmAdminApiTest, TokenRequired) {
    ops::GmConsole console;
    ops::GmAdminApi api(&console, "secret");
    http::HttpRequest req = make_req("GET", "/gm/health");
    http::HttpResponse resp;
    api.handle(req, resp);
    EXPECT_EQ(401, resp.status_code);

    req.headers["x-gm-token"] = "secret";
    api.handle(req, resp);
    EXPECT_EQ(200, resp.status_code);
}

TEST(GmAdminApiTest, ListCommands) {
    ops::GmConsole console;
    console.register_command("kick", 0,
        [](const std::string&, const std::string&, std::string& r) { r = "ok"; return true; });
    console.register_command("ban", 0,
        [](const std::string&, const std::string&, std::string& r) { r = "ok"; return true; });

    ops::GmAdminApi api(&console);
    http::HttpRequest req = make_req("GET", "/gm/commands");
    http::HttpResponse resp;
    api.handle(req, resp);
    EXPECT_EQ(200, resp.status_code);
    EXPECT_NE(std::string::npos, resp.body.find("\"kick\""));
    EXPECT_NE(std::string::npos, resp.body.find("\"ban\""));
}

TEST(GmAdminApiTest, InvokeViaJsonBody) {
    ops::GmConsole console;
    console.register_command("echo", 0,
        [](const std::string&, const std::string& args, std::string& r) {
            r = args;
            return true;
        });
    console.set_actor_level("root", 0);

    ops::GmAdminApi api(&console);
    http::HttpRequest req = make_req("POST", "/gm/invoke",
        R"({"actor":"root","command":"echo","args":"hello"})");
    http::HttpResponse resp;
    api.handle(req, resp);
    EXPECT_EQ(200, resp.status_code);
    EXPECT_NE(std::string::npos, resp.body.find("\"ok\":true"));
    EXPECT_NE(std::string::npos, resp.body.find("hello"));
}

TEST(GmAdminApiTest, InvokeMissingArgs) {
    ops::GmConsole console;
    ops::GmAdminApi api(&console);
    http::HttpRequest req = make_req("POST", "/gm/invoke", "{}");
    http::HttpResponse resp;
    api.handle(req, resp);
    EXPECT_NE(std::string::npos, resp.body.find("required"));
}

TEST(GmAdminApiTest, AuditListed) {
    ops::GmConsole console;
    console.register_command("ping", 0,
        [](const std::string&, const std::string&, std::string& r) { r = "pong"; return true; });
    console.set_actor_level("a", 0);
    std::string out;
    console.invoke("a", "ping", "", out, 42);

    ops::GmAdminApi api(&console);
    http::HttpRequest req = make_req("GET", "/gm/audit?limit=10");
    http::HttpResponse resp;
    api.handle(req, resp);
    EXPECT_EQ(200, resp.status_code);
    EXPECT_NE(std::string::npos, resp.body.find("ping"));
    EXPECT_NE(std::string::npos, resp.body.find("pong"));
}

TEST(GmAdminApiTest, NotFound) {
    ops::GmConsole console;
    ops::GmAdminApi api(&console);
    http::HttpRequest req = make_req("GET", "/nope");
    http::HttpResponse resp;
    api.handle(req, resp);
    EXPECT_EQ(404, resp.status_code);
}

// ==================== Channel adapters ====================

TEST(WechatPayAdapterTest, CreateOrderAndPrepay) {
    pay::ChannelConfig cfg;
    cfg.app_id = "wx_app";
    cfg.mch_id = "mch_1";
    cfg.notify_url = "http://cb";
    pay::WechatPayAdapter gw(cfg,
        [](const std::string& p) { return "S" + p; },
        [](const std::string& p, const std::string& s) { return s == "S" + p; });

    auto o = gw.create_order("u1", "sku", 100, "CNY", 1000);
    EXPECT_FALSE(o.order_id.empty());
    std::string prepay;
    ASSERT_TRUE(gw.prepay_params(o.order_id, prepay));
    EXPECT_NE(std::string::npos, prepay.find("prepay_id"));
    EXPECT_NE(std::string::npos, prepay.find("Sappid="));
}

TEST(WechatPayAdapterTest, CallbackVerified) {
    pay::ChannelConfig cfg;
    cfg.app_id = "a";
    cfg.mch_id = "m";
    pay::WechatPayAdapter gw(cfg,
        [](const std::string& p) { return "S" + p; },
        [](const std::string& p, const std::string& s) { return s == "S" + p; });

    auto o = gw.create_order("u1", "sku", 100, "CNY", 1);
    pay::PaymentCallback cb;
    cb.order_id = o.order_id;
    cb.amount_cents = 100;
    cb.transaction_id = "wx_123";
    std::string payload = "out_trade_no=" + cb.order_id
                        + "&transaction_id=" + cb.transaction_id
                        + "&total_fee=" + std::to_string(cb.amount_cents);
    cb.sign = "S" + payload;

    std::string msg;
    EXPECT_TRUE(gw.handle_callback(cb, msg));
    pay::PaymentOrder got;
    ASSERT_TRUE(gw.query_order(o.order_id, got));
    EXPECT_EQ(pay::OrderStatus::Paid, got.status);
}

TEST(WechatPayAdapterTest, BadSignRejected) {
    pay::ChannelConfig cfg;
    cfg.app_id = "a";
    cfg.mch_id = "m";
    pay::WechatPayAdapter gw(cfg,
        [](const std::string& p) { return "S" + p; },
        [](const std::string& p, const std::string& s) { return s == "S" + p; });

    auto o = gw.create_order("u1", "sku", 100, "CNY", 1);
    pay::PaymentCallback cb;
    cb.order_id = o.order_id;
    cb.amount_cents = 100;
    cb.sign = "wrong";
    std::string msg;
    EXPECT_FALSE(gw.handle_callback(cb, msg));
    EXPECT_EQ("sign verify failed", msg);
}

TEST(AlipayAdapterTest, CreateAndPay) {
    pay::ChannelConfig cfg;
    cfg.app_id = "ali_app";
    cfg.notify_url = "http://cb";
    pay::AlipayAdapter gw(cfg,
        [](const std::string& p) { return "A" + p; },
        [](const std::string& p, const std::string& s) { return s == "A" + p; });

    auto o = gw.create_order("u1", "sku", 200, "CNY", 1);
    EXPECT_FALSE(o.order_id.empty());
    std::string form;
    ASSERT_TRUE(gw.trade_form(o.order_id, form));
    EXPECT_NE(std::string::npos, form.find("alipay_gateway"));

    pay::PaymentCallback cb;
    cb.order_id = o.order_id;
    cb.amount_cents = 200;
    cb.transaction_id = "ali_1";
    std::string payload = "out_trade_no=" + cb.order_id
                        + "&trade_no=" + cb.transaction_id
                        + "&total_amount=" + std::to_string(cb.amount_cents);
    cb.sign = "A" + payload;
    std::string msg;
    EXPECT_TRUE(gw.handle_callback(cb, msg));
    pay::PaymentOrder got;
    ASSERT_TRUE(gw.query_order(o.order_id, got));
    EXPECT_EQ(pay::OrderStatus::Paid, got.status);
}

TEST(AlipayAdapterTest, NoVerifierFails) {
    pay::ChannelConfig cfg;
    cfg.app_id = "a";
    pay::AlipayAdapter gw(cfg, nullptr, nullptr);
    auto o = gw.create_order("u1", "sku", 10, "CNY", 1);
    pay::PaymentCallback cb;
    cb.order_id = o.order_id;
    cb.amount_cents = 10;
    std::string msg;
    EXPECT_FALSE(gw.handle_callback(cb, msg));
}
