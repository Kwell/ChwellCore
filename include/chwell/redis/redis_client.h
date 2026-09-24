#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <memory>
#include <functional>
#include <mutex>
#include <deque>

#include "chwell/core/logger.h"
#include "chwell/service/component.h"

namespace chwell {
namespace redis {

// Redis 配置
struct RedisConfig {
    std::string host;
    int port;
    std::string password;
    int db;
    int connection_timeout_ms;
    int read_timeout_ms;
    int max_connections;
    
    RedisConfig()
        : host("127.0.0.1"), port(6379), db(0)
        , connection_timeout_ms(5000), read_timeout_ms(3000)
        , max_connections(10) {}
};

// Redis 回复类型
enum class ReplyType {
    STRING, INTEGER, ARRAY, NIL, ERROR, STATUS
};

// Redis 回复
struct RedisReply {
    ReplyType type;
    std::string str;
    int64_t integer;
    std::vector<RedisReply> elements;
    
    RedisReply() : type(ReplyType::NIL), integer(0) {}
    
    bool ok() const { return type != ReplyType::ERROR && type != ReplyType::NIL; }
    bool is_string() const { return type == ReplyType::STRING; }
    bool is_integer() const { return type == ReplyType::INTEGER; }
    bool is_array() const { return type == ReplyType::ARRAY; }
    bool is_nil() const { return type == ReplyType::NIL; }
    bool is_error() const { return type == ReplyType::ERROR; }
};

// Redis 客户端（简化实现，可用 hiredis 替换）
class RedisClient {
public:
    using Ptr = std::shared_ptr<RedisClient>;
    
    explicit RedisClient(const RedisConfig& config = RedisConfig());
    ~RedisClient();
    
    bool connect();
    void disconnect();
    bool is_connected() const;
    
    // 当前是否为内存模拟（非真实 Redis 连接）
    // 生产环境部署前必须替换为真实实现
    // true = 内存 mock；false = 已建立 RESP/TCP 真连接
    bool is_mock() const { return use_mock_; }
    
    // 字符串操作
    bool set(const std::string& key, const std::string& value);
    bool setex(const std::string& key, int seconds, const std::string& value);
    bool setnx(const std::string& key, const std::string& value);
    
    // 原子操作（分布式锁安全的基础）
    // SET key value NX EX seconds —— 原子"不存在则设置并带过期"
    bool set_nx_ex(const std::string& key, const std::string& value, int seconds);
    // 原子 CAS+DEL：仅当 key 的值 == expected 时才删除，返回是否删除成功
    bool compare_and_del(const std::string& key, const std::string& expected);
    // 原子 CAS+EXPIRE：仅当 key 的值 == expected 时才设置过期
    bool compare_and_expire(const std::string& key, const std::string& expected, int seconds);
    
    std::string get(const std::string& key);
    bool get(const std::string& key, std::string& value);
    int del(const std::string& key);
    bool exists(const std::string& key);
    bool expire(const std::string& key, int seconds);
    int ttl(const std::string& key);
    int64_t incr(const std::string& key);
    int64_t incrby(const std::string& key, int64_t increment);
    
    // Hash 操作
    bool hset(const std::string& key, const std::string& field, const std::string& value);
    std::string hget(const std::string& key, const std::string& field);
    bool hget(const std::string& key, const std::string& field, std::string& value);
    bool hexists(const std::string& key, const std::string& field);
    int hdel(const std::string& key, const std::string& field);
    int64_t hlen(const std::string& key);
    std::unordered_map<std::string, std::string> hgetall(const std::string& key);
    
    // List 操作
    int64_t lpush(const std::string& key, const std::string& value);
    int64_t rpush(const std::string& key, const std::string& value);
    std::string lpop(const std::string& key);
    std::string rpop(const std::string& key);
    int64_t llen(const std::string& key);
    std::vector<std::string> lrange(const std::string& key, int64_t start, int64_t stop);
    
    // Set 操作
    int sadd(const std::string& key, const std::string& member);
    int srem(const std::string& key, const std::string& member);
    bool sismember(const std::string& key, const std::string& member);
    int64_t scard(const std::string& key);
    std::vector<std::string> smembers(const std::string& key);
    
    // Sorted Set 操作
    int zadd(const std::string& key, double score, const std::string& member);
    int zrem(const std::string& key, const std::string& member);
    double zscore(const std::string& key, const std::string& member);
    int64_t zrank(const std::string& key, const std::string& member);
    int64_t zcard(const std::string& key);
    std::vector<std::string> zrange(const std::string& key, int64_t start, int64_t stop);
    
    // 原始命令
    RedisReply execute(const std::vector<std::string>& args);
    
    const RedisConfig& config() const { return config_; }
    
private:
    RedisConfig config_;
    mutable std::mutex mutex_;
    bool connected_{false};  // 连接状态标志
    bool use_mock_{true};    // true=内存 mock，false=RESP/TCP
    int fd_{-1};             // RESP 连接 fd
    std::string rbuf_;       // RESP 读缓冲
    
    // 内存模拟存储（无 hiredis 时使用）
    std::unordered_map<std::string, std::string> data_;
    std::unordered_map<std::string, int64_t> expires_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> hashes_;
    std::unordered_map<std::string, std::deque<std::string>> lists_;
    std::unordered_map<std::string, std::set<std::string>> sets_;
    std::unordered_map<std::string, std::vector<std::pair<double, std::string>>> zsets_;
};

// Redis 缓存组件
class RedisCacheComponent : public service::Component {
public:
    explicit RedisCacheComponent(const RedisConfig& config = RedisConfig())
        : config_(config) {}
    
    virtual std::string name() const override {
        return "RedisCacheComponent";
    }
    
    virtual void on_register(service::Service& svc) override {
        client_ = std::make_shared<RedisClient>(config_);
        client_->connect();
    }
    
    RedisClient::Ptr client() const { return client_; }
    
private:
    RedisConfig config_;
    RedisClient::Ptr client_;
};

} // namespace redis
} // namespace chwell