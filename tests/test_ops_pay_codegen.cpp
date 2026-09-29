#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "chwell/ops/analytics.h"
#include "chwell/ops/gm_console.h"
#include "chwell/pay/payment_gateway.h"
#include "chwell/codegen/schema_codegen.h"

using namespace chwell;

// ==================== GmConsole ====================

TEST(GmConsoleTest, RegisterAndInvoke) {
    ops::GmConsole console;
    console.register_command("kick", 0,
        [](const std::string&, const std::string& args, std::string& result) {
            result = "kicked " + args;
            return true;
        });
    console.set_actor_level("admin", 0);

    std::string out;
    ASSERT_TRUE(console.invoke("admin", "kick", "player1", out));
    EXPECT_EQ("kicked player1", out);
}

TEST(GmConsoleTest, PermissionDenied) {
    ops::GmConsole console;
    console.register_command("ban", 10,
        [](const std::string&, const std::string&, std::string& r) { r = "ok"; return true; });
    console.set_actor_level("gm", 1);

    std::string out;
    EXPECT_FALSE(console.invoke("gm", "ban", "x", out));
    EXPECT_EQ("permission denied", out);
}

TEST(GmConsoleTest, UnknownCommand) {
    ops::GmConsole console;
    std::string out;
    EXPECT_FALSE(console.invoke("admin", "nope", "", out));
    EXPECT_EQ("unknown command", out);
}

TEST(GmConsoleTest, AuditRecorded) {
    ops::GmConsole console;
    console.register_command("hello", 0,
        [](const std::string&, const std::string&, std::string& r) { r = "hi"; return true; });
    console.set_actor_level("a", 0);
    std::string out;
    console.invoke("a", "hello", "x", out, /*now=*/123);

    auto log = console.audit_log();
    ASSERT_EQ(1u, log.size());
    EXPECT_EQ("a", log[0].actor);
    EXPECT_EQ("hello", log[0].command);
    EXPECT_TRUE(log[0].ok);
    EXPECT_EQ(123, log[0].time_ms);
}

TEST(GmConsoleTest, HandlerExceptionCaught) {
    ops::GmConsole console;
    console.register_command("boom", 0,
        [](const std::string&, const std::string&, std::string&) -> bool {
            throw std::runtime_error("x");
        });
    console.set_actor_level("a", 0);
    std::string out;
    EXPECT_FALSE(console.invoke("a", "boom", "", out));
    EXPECT_EQ("handler exception", out);
}

TEST(GmConsoleTest, Unregister) {
    ops::GmConsole console;
    console.register_command("x", 0,
        [](const std::string&, const std::string&, std::string& r) { return true; });
    EXPECT_TRUE(console.has_command("x"));
    EXPECT_TRUE(console.unregister_command("x"));
    EXPECT_FALSE(console.has_command("x"));
}

// ==================== AnalyticsPipeline ====================

TEST(AnalyticsTest, CountAndUnique) {
    ops::AnalyticsPipeline ap;
    ap.track("login", "u1", 100);
    ap.track("login", "u2", 150);
    ap.track("login", "u1", 200);
    ap.track("logout", "u1", 300);

    EXPECT_EQ(3u, ap.count("login", 0, 1000));
    EXPECT_EQ(2u, ap.unique_users("login", 0, 1000));
    EXPECT_EQ(1u, ap.count("logout", 0, 1000));
    EXPECT_EQ(0u, ap.count("login", 500, 1000));
}

TEST(AnalyticsTest, TopEvents) {
    ops::AnalyticsPipeline ap;
    ap.track("a", "u", 1);
    ap.track("a", "u", 2);
    ap.track("b", "u", 3);
    auto top = ap.top_events(3);
    ASSERT_GE(top.size(), 2u);
    EXPECT_EQ("a", top[0].first);
    EXPECT_EQ(2u, top[0].second);
}

TEST(AnalyticsTest, FunnelCounts) {
    ops::AnalyticsPipeline ap;
    // u1 完成全部三步
    ap.track("view", "u1", 10);
    ap.track("click", "u1", 20);
    ap.track("pay", "u1", 30);
    // u2 只完成前两步
    ap.track("view", "u2", 11);
    ap.track("click", "u2", 21);
    // u3 顺序颠倒（pay 在 click 前）→ 不算完成
    ap.track("view", "u3", 12);
    ap.track("pay", "u3", 15);
    ap.track("click", "u3", 25);

    auto f = ap.funnel({"view", "click", "pay"}, 0, 100);
    ASSERT_EQ(3u, f.size());
    // 只有 u1 按序完成全部
    EXPECT_EQ(1u, f[0]);
    EXPECT_EQ(1u, f[1]);
    EXPECT_EQ(1u, f[2]);
}

TEST(AnalyticsTest, MaxEventsEvictsOldest) {
    ops::AnalyticsPipeline ap(3);
    for (int i = 0; i < 5; ++i) ap.track("e", "u", i);
    EXPECT_EQ(3u, ap.total_events());
}

// ==================== MemoryPaymentGateway ====================

TEST(PaymentTest, CreateOrder) {
    pay::MemoryPaymentGateway gw;
    auto o = gw.create_order("u1", "sku_100", 990, "CNY", 1000);
    EXPECT_FALSE(o.order_id.empty());
    EXPECT_EQ(pay::OrderStatus::Created, o.status);
    EXPECT_EQ(990, o.amount_cents);
    EXPECT_EQ(1u, gw.order_count());
}

TEST(PaymentTest, CreateOrderRejectsInvalid) {
    pay::MemoryPaymentGateway gw;
    EXPECT_TRUE(gw.create_order("", "sku", 10, "CNY", 1).order_id.empty());
    EXPECT_TRUE(gw.create_order("u", "", 10, "CNY", 1).order_id.empty());
    EXPECT_TRUE(gw.create_order("u", "sku", 0, "CNY", 1).order_id.empty());
    EXPECT_TRUE(gw.create_order("u", "sku", -5, "CNY", 1).order_id.empty());
}

TEST(PaymentTest, CallbackPaysOrder) {
    pay::MemoryPaymentGateway gw;
    auto o = gw.create_order("u1", "sku", 100, "CNY", 1000);

    pay::PaymentCallback cb;
    cb.order_id = o.order_id;
    cb.amount_cents = 100;
    cb.sign = "ok_" + o.order_id;
    cb.success = true;

    std::string msg;
    EXPECT_TRUE(gw.handle_callback(cb, msg));
    pay::PaymentOrder got;
    ASSERT_TRUE(gw.query_order(o.order_id, got));
    EXPECT_EQ(pay::OrderStatus::Paid, got.status);
}

TEST(PaymentTest, BadSignRejected) {
    pay::MemoryPaymentGateway gw;
    auto o = gw.create_order("u1", "sku", 100, "CNY", 1);
    pay::PaymentCallback cb;
    cb.order_id = o.order_id;
    cb.amount_cents = 100;
    cb.sign = "bad";
    std::string msg;
    EXPECT_FALSE(gw.handle_callback(cb, msg));
    EXPECT_EQ("bad sign", msg);
}

TEST(PaymentTest, AmountMismatchRejected) {
    pay::MemoryPaymentGateway gw;
    auto o = gw.create_order("u1", "sku", 100, "CNY", 1);
    pay::PaymentCallback cb;
    cb.order_id = o.order_id;
    cb.amount_cents = 50;  // 与订单不符
    cb.sign = "ok_" + o.order_id;
    std::string msg;
    EXPECT_FALSE(gw.handle_callback(cb, msg));
    EXPECT_EQ("amount mismatch", msg);
}

TEST(PaymentTest, DuplicateCallbackIdempotent) {
    pay::MemoryPaymentGateway gw;
    auto o = gw.create_order("u1", "sku", 100, "CNY", 1);
    pay::PaymentCallback cb;
    cb.order_id = o.order_id;
    cb.amount_cents = 100;
    cb.sign = "ok_" + o.order_id;

    std::string msg;
    EXPECT_TRUE(gw.handle_callback(cb, msg));
    EXPECT_TRUE(gw.handle_callback(cb, msg));  // 幂等
    EXPECT_EQ("already paid", msg);
}

TEST(PaymentTest, Refund) {
    pay::MemoryPaymentGateway gw;
    auto o = gw.create_order("u1", "sku", 100, "CNY", 1);
    pay::PaymentCallback cb;
    cb.order_id = o.order_id;
    cb.amount_cents = 100;
    cb.sign = "ok_" + o.order_id;
    std::string msg;
    ASSERT_TRUE(gw.handle_callback(cb, msg));
    EXPECT_TRUE(gw.refund(o.order_id));
    pay::PaymentOrder got;
    ASSERT_TRUE(gw.query_order(o.order_id, got));
    EXPECT_EQ(pay::OrderStatus::Refunded, got.status);
}

TEST(PaymentTest, OrderStatusNames) {
    EXPECT_STREQ("created", pay::order_status_name(pay::OrderStatus::Created));
    EXPECT_STREQ("paid", pay::order_status_name(pay::OrderStatus::Paid));
}

// ==================== SchemaCodegen ====================

TEST(CodegenTest, GeneratesClassSkeleton) {
    codegen::SchemaCodegen gen;
    gen.add_field("id", "string");
    gen.add_field("level", "int");
    gen.add_field("gold", "int64");

    std::string src = gen.generate_entity("Player", "players");
    EXPECT_NE(std::string::npos, src.find("class Player"));
    EXPECT_NE(std::string::npos, src.find("return \"players\";"));
    EXPECT_NE(std::string::npos, src.find("CHWELL_FIELD(std::string, id_, id)"));
    EXPECT_NE(std::string::npos, src.find("CHWELL_FIELD(int, level_, level)"));
    EXPECT_NE(std::string::npos, src.find("d.set_int64(\"gold\", gold_)"));
    EXPECT_NE(std::string::npos, src.find("clear_dirty();"));
}

TEST(CodegenTest, SampleJson) {
    codegen::SchemaCodegen gen;
    gen.add_field("name", "string");
    gen.add_field("ok", "bool");
    std::string json = gen.generate_sample_json();
    EXPECT_NE(std::string::npos, json.find("\"name\""));
    EXPECT_NE(std::string::npos, json.find("\"ok\": true"));
}

TEST(CodegenTest, ClearResets) {
    codegen::SchemaCodegen gen;
    gen.add_field("a", "int");
    EXPECT_EQ(1u, gen.fields().size());
    gen.clear();
    EXPECT_EQ(0u, gen.fields().size());
}
