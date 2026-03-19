#include "chwell/redis/redis_client.h"
#include <chrono>
#include <set>
#include <algorithm>

namespace chwell {
namespace redis {

RedisClient::RedisClient(const RedisConfig& config)
    : config_(config) {
}

RedisClient::~RedisClient() {
    disconnect();
}

bool RedisClient::connect() {
    // 内存模拟实现，始终返回 true
    CHWELL_LOG_INFO("RedisClient (memory mock) connected");
    return true;
}

void RedisClient::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    CHWELL_LOG_INFO("RedisClient (memory mock) disconnected");
    data_.clear();
    expires_.clear();
    hashes_.clear();
    lists_.clear();
    sets_.clear();
    zsets_.clear();
}

bool RedisClient::is_connected() const {
    return true;
}

RedisReply RedisClient::execute(const std::vector<std::string>& args) {
    std::lock_guard<std::mutex> lock(mutex_);
    RedisReply reply;
    
    if (args.empty()) {
        reply.type = ReplyType::ERROR;
        reply.str = "empty command";
        return reply;
    }
    
    std::string cmd = args[0];
    for (char& c : cmd) c = std::toupper(c);
    
    // 检查过期
    auto check_expire = [this](const std::string& key) {
        auto it = expires_.find(key);
        if (it != expires_.end()) {
            auto now = std::chrono::steady_clock::now().time_since_epoch().count() / 1000000000;
            if (now >= it->second) {
                data_.erase(key);
                expires_.erase(key);
                hashes_.erase(key);
                lists_.erase(key);
                sets_.erase(key);
                zsets_.erase(key);
                return true;
            }
        }
        return false;
    };
    
    if (cmd == "SET" && args.size() >= 3) {
        data_[args[1]] = args[2];
        reply.type = ReplyType::STATUS;
        reply.str = "OK";
    }
    else if (cmd == "SETEX" && args.size() >= 4) {
        data_[args[1]] = args[3];
        auto now = std::chrono::steady_clock::now().time_since_epoch().count() / 1000000000;
        expires_[args[1]] = now + std::stoll(args[2]);
        reply.type = ReplyType::STATUS;
        reply.str = "OK";
    }
    else if (cmd == "SETNX" && args.size() >= 3) {
        if (data_.find(args[1]) == data_.end()) {
            data_[args[1]] = args[2];
            reply.type = ReplyType::INTEGER;
            reply.integer = 1;
        } else {
            reply.type = ReplyType::INTEGER;
            reply.integer = 0;
        }
    }
    else if (cmd == "GET" && args.size() >= 2) {
        check_expire(args[1]);
        auto it = data_.find(args[1]);
        if (it != data_.end()) {
            reply.type = ReplyType::STRING;
            reply.str = it->second;
        } else {
            reply.type = ReplyType::NIL;
        }
    }
    else if (cmd == "DEL" && args.size() >= 2) {
        int count = 0;
        for (size_t i = 1; i < args.size(); ++i) {
            count += data_.erase(args[i]);
            expires_.erase(args[i]);
            hashes_.erase(args[i]);
            lists_.erase(args[i]);
            sets_.erase(args[i]);
            zsets_.erase(args[i]);
        }
        reply.type = ReplyType::INTEGER;
        reply.integer = count;
    }
    else if (cmd == "EXISTS" && args.size() >= 2) {
        check_expire(args[1]);
        reply.type = ReplyType::INTEGER;
        reply.integer = data_.count(args[1]) ? 1 : 0;
    }
    else if (cmd == "EXPIRE" && args.size() >= 3) {
        if (data_.count(args[1])) {
            auto now = std::chrono::steady_clock::now().time_since_epoch().count() / 1000000000;
            expires_[args[1]] = now + std::stoll(args[2]);
            reply.type = ReplyType::INTEGER;
            reply.integer = 1;
        } else {
            reply.type = ReplyType::INTEGER;
            reply.integer = 0;
        }
    }
    else if (cmd == "TTL" && args.size() >= 2) {
        if (!data_.count(args[1])) {
            reply.type = ReplyType::INTEGER;
            reply.integer = -2;
        } else if (!expires_.count(args[1])) {
            reply.type = ReplyType::INTEGER;
            reply.integer = -1;
        } else {
            auto now = std::chrono::steady_clock::now().time_since_epoch().count() / 1000000000;
            reply.type = ReplyType::INTEGER;
            reply.integer = std::max((int64_t)0, expires_[args[1]] - now);
        }
    }
    else if (cmd == "INCR" && args.size() >= 2) {
        int64_t val = data_.count(args[1]) ? std::stoll(data_[args[1]]) : 0;
        data_[args[1]] = std::to_string(++val);
        reply.type = ReplyType::INTEGER;
        reply.integer = val;
    }
    else if (cmd == "INCRBY" && args.size() >= 3) {
        int64_t val = data_.count(args[1]) ? std::stoll(data_[args[1]]) : 0;
        val += std::stoll(args[2]);
        data_[args[1]] = std::to_string(val);
        reply.type = ReplyType::INTEGER;
        reply.integer = val;
    }
    else if (cmd == "HSET" && args.size() >= 4) {
        hashes_[args[1]][args[2]] = args[3];
        reply.type = ReplyType::INTEGER;
        reply.integer = 1;
    }
    else if (cmd == "HGET" && args.size() >= 3) {
        auto it = hashes_.find(args[1]);
        if (it != hashes_.end() && it->second.count(args[2])) {
            reply.type = ReplyType::STRING;
            reply.str = it->second[args[2]];
        } else {
            reply.type = ReplyType::NIL;
        }
    }
    else if (cmd == "HGETALL" && args.size() >= 2) {
        reply.type = ReplyType::ARRAY;
        auto it = hashes_.find(args[1]);
        if (it != hashes_.end()) {
            for (const auto& p : it->second) {
                RedisReply f, v;
                f.type = ReplyType::STRING; f.str = p.first;
                v.type = ReplyType::STRING; v.str = p.second;
                reply.elements.push_back(f);
                reply.elements.push_back(v);
            }
        }
    }
    else if (cmd == "HEXISTS" && args.size() >= 3) {
        auto it = hashes_.find(args[1]);
        if (it != hashes_.end() && it->second.count(args[2])) {
            reply.type = ReplyType::INTEGER;
            reply.integer = 1;
        } else {
            reply.type = ReplyType::INTEGER;
            reply.integer = 0;
        }
    }
    else if (cmd == "HDEL" && args.size() >= 3) {
        auto it = hashes_.find(args[1]);
        if (it != hashes_.end() && it->second.erase(args[2])) {
            reply.type = ReplyType::INTEGER;
            reply.integer = 1;
        } else {
            reply.type = ReplyType::INTEGER;
            reply.integer = 0;
        }
    }
    else if (cmd == "HLEN" && args.size() >= 2) {
        auto it = hashes_.find(args[1]);
        reply.type = ReplyType::INTEGER;
        reply.integer = it != hashes_.end() ? (int64_t)it->second.size() : 0;
    }
    else if (cmd == "LPUSH" && args.size() >= 3) {
        lists_[args[1]].push_front(args[2]);
        reply.type = ReplyType::INTEGER;
        reply.integer = lists_[args[1]].size();
    }
    else if (cmd == "RPUSH" && args.size() >= 3) {
        lists_[args[1]].push_back(args[2]);
        reply.type = ReplyType::INTEGER;
        reply.integer = lists_[args[1]].size();
    }
    else if (cmd == "LPOP" && args.size() >= 2) {
        auto it = lists_.find(args[1]);
        if (it != lists_.end() && !it->second.empty()) {
            reply.type = ReplyType::STRING;
            reply.str = it->second.front();
            it->second.pop_front();
        } else {
            reply.type = ReplyType::NIL;
        }
    }
    else if (cmd == "RPOP" && args.size() >= 2) {
        auto it = lists_.find(args[1]);
        if (it != lists_.end() && !it->second.empty()) {
            reply.type = ReplyType::STRING;
            reply.str = it->second.back();
            it->second.pop_back();
        } else {
            reply.type = ReplyType::NIL;
        }
    }
    else if (cmd == "LRANGE" && args.size() >= 4) {
        reply.type = ReplyType::ARRAY;
        auto it = lists_.find(args[1]);
        if (it != lists_.end()) {
            int64_t start = std::stoll(args[2]);
            int64_t stop = std::stoll(args[3]);
            auto& lst = it->second;
            int64_t len = lst.size();
            if (start < 0) start = len + start;
            if (stop < 0) stop = len + stop;
            start = std::max((int64_t)0, start);
            stop = std::min(len - 1, stop);
            for (int64_t i = start; i <= stop; ++i) {
                RedisReply item;
                item.type = ReplyType::STRING;
                item.str = lst[i];
                reply.elements.push_back(item);
            }
        }
    }
    else if (cmd == "SADD" && args.size() >= 3) {
        reply.type = ReplyType::INTEGER;
        reply.integer = sets_[args[1]].insert(args[2]).second ? 1 : 0;
    }
    else if (cmd == "SREM" && args.size() >= 3) {
        reply.type = ReplyType::INTEGER;
        reply.integer = sets_[args[1]].erase(args[2]);
    }
    else if (cmd == "SISMEMBER" && args.size() >= 3) {
        reply.type = ReplyType::INTEGER;
        reply.integer = sets_[args[1]].count(args[2]) ? 1 : 0;
    }
    else if (cmd == "SMEMBERS" && args.size() >= 2) {
        reply.type = ReplyType::ARRAY;
        auto it = sets_.find(args[1]);
        if (it != sets_.end()) {
            for (const auto& m : it->second) {
                RedisReply item;
                item.type = ReplyType::STRING;
                item.str = m;
                reply.elements.push_back(item);
            }
        }
    }
    else if (cmd == "SCARD" && args.size() >= 2) {
        auto it = sets_.find(args[1]);
        reply.type = ReplyType::INTEGER;
        reply.integer = it != sets_.end() ? (int64_t)it->second.size() : 0;
    }
    else if (cmd == "ZADD" && args.size() >= 4) {
        double score = std::stod(args[2]);
        auto& zset = zsets_[args[1]];
        bool found = false;
        for (auto& p : zset) {
            if (p.second == args[3]) {
                p.first = score;
                found = true;
                break;
            }
        }
        if (!found) {
            zset.push_back({score, args[3]});
            std::sort(zset.begin(), zset.end());
        }
        reply.type = ReplyType::INTEGER;
        reply.integer = found ? 0 : 1;
    }
    else if (cmd == "ZRANGE" && args.size() >= 4) {
        reply.type = ReplyType::ARRAY;
        auto it = zsets_.find(args[1]);
        if (it != zsets_.end()) {
            int64_t start = std::stoll(args[2]);
            int64_t stop = std::stoll(args[3]);
            auto& zset = it->second;
            int64_t len = zset.size();
            if (start < 0) start = len + start;
            if (stop < 0) stop = len + stop;
            start = std::max((int64_t)0, start);
            stop = std::min(len - 1, stop);
            for (int64_t i = start; i <= stop; ++i) {
                RedisReply item;
                item.type = ReplyType::STRING;
                item.str = zset[i].second;
                reply.elements.push_back(item);
            }
        }
    }
    else if (cmd == "ZREM" && args.size() >= 3) {
        auto it = zsets_.find(args[1]);
        reply.type = ReplyType::INTEGER;
        reply.integer = 0;
        if (it != zsets_.end()) {
            for (auto z_it = it->second.begin(); z_it != it->second.end(); ++z_it) {
                if (z_it->second == args[2]) {
                    it->second.erase(z_it);
                    reply.integer = 1;
                    break;
                }
            }
        }
    }
    else if (cmd == "ZSCORE" && args.size() >= 3) {
        auto it = zsets_.find(args[1]);
        if (it != zsets_.end()) {
            for (const auto& p : it->second) {
                if (p.second == args[2]) {
                    reply.type = ReplyType::STRING;
                    reply.str = std::to_string(p.first);
                    break;
                }
            }
        }
        if (reply.type != ReplyType::STRING) {
            reply.type = ReplyType::NIL;
        }
    }
    else if (cmd == "ZRANK" && args.size() >= 3) {
        auto it = zsets_.find(args[1]);
        reply.type = ReplyType::INTEGER;
        reply.integer = -1;
        if (it != zsets_.end()) {
            for (size_t i = 0; i < it->second.size(); ++i) {
                if (it->second[i].second == args[2]) {
                    reply.integer = (int64_t)i;
                    break;
                }
            }
        }
    }
    else if (cmd == "ZCARD" && args.size() >= 2) {
        auto it = zsets_.find(args[1]);
        reply.type = ReplyType::INTEGER;
        reply.integer = it != zsets_.end() ? (int64_t)it->second.size() : 0;
    }
    else if (cmd == "LLEN" && args.size() >= 2) {
        auto it = lists_.find(args[1]);
        reply.type = ReplyType::INTEGER;
        reply.integer = it != lists_.end() ? (int64_t)it->second.size() : 0;
    }
    else if (cmd == "PING") {
        reply.type = ReplyType::STATUS;
        reply.str = "PONG";
    }
    else {
        reply.type = ReplyType::ERROR;
        reply.str = "unsupported command: " + cmd;
    }
    
    return reply;
}

// 简化实现其他方法...
bool RedisClient::set(const std::string& key, const std::string& value) {
    return execute({"SET", key, value}).ok();
}

bool RedisClient::setex(const std::string& key, int seconds, const std::string& value) {
    return execute({"SETEX", key, std::to_string(seconds), value}).ok();
}

bool RedisClient::setnx(const std::string& key, const std::string& value) {
    auto r = execute({"SETNX", key, value});
    return r.is_integer() && r.integer == 1;
}

std::string RedisClient::get(const std::string& key) {
    auto r = execute({"GET", key});
    return r.is_string() ? r.str : "";
}

bool RedisClient::get(const std::string& key, std::string& value) {
    auto r = execute({"GET", key});
    if (r.is_string()) { value = r.str; return true; }
    return false;
}

int RedisClient::del(const std::string& key) {
    auto r = execute({"DEL", key});
    return r.is_integer() ? (int)r.integer : 0;
}

bool RedisClient::exists(const std::string& key) {
    auto r = execute({"EXISTS", key});
    return r.is_integer() && r.integer == 1;
}

bool RedisClient::expire(const std::string& key, int seconds) {
    auto r = execute({"EXPIRE", key, std::to_string(seconds)});
    return r.is_integer() && r.integer == 1;
}

int RedisClient::ttl(const std::string& key) {
    auto r = execute({"TTL", key});
    return r.is_integer() ? (int)r.integer : -2;
}

int64_t RedisClient::incr(const std::string& key) {
    auto r = execute({"INCR", key});
    return r.is_integer() ? r.integer : 0;
}

int64_t RedisClient::incrby(const std::string& key, int64_t increment) {
    auto r = execute({"INCRBY", key, std::to_string(increment)});
    return r.is_integer() ? r.integer : 0;
}

bool RedisClient::hset(const std::string& key, const std::string& field, const std::string& value) {
    return execute({"HSET", key, field, value}).ok();
}

std::string RedisClient::hget(const std::string& key, const std::string& field) {
    auto r = execute({"HGET", key, field});
    return r.is_string() ? r.str : "";
}

bool RedisClient::hget(const std::string& key, const std::string& field, std::string& value) {
    auto r = execute({"HGET", key, field});
    if (r.is_string()) { value = r.str; return true; }
    return false;
}

bool RedisClient::hexists(const std::string& key, const std::string& field) {
    auto r = execute({"HEXISTS", key, field});
    return r.is_integer() && r.integer == 1;
}

int RedisClient::hdel(const std::string& key, const std::string& field) {
    auto r = execute({"HDEL", key, field});
    return r.is_integer() ? (int)r.integer : 0;
}

int64_t RedisClient::hlen(const std::string& key) {
    auto r = execute({"HLEN", key});
    return r.is_integer() ? r.integer : 0;
}

std::unordered_map<std::string, std::string> RedisClient::hgetall(const std::string& key) {
    std::unordered_map<std::string, std::string> result;
    auto r = execute({"HGETALL", key});
    if (r.is_array() && r.elements.size() % 2 == 0) {
        for (size_t i = 0; i < r.elements.size(); i += 2) {
            result[r.elements[i].str] = r.elements[i+1].str;
        }
    }
    return result;
}

int64_t RedisClient::lpush(const std::string& key, const std::string& value) {
    auto r = execute({"LPUSH", key, value});
    return r.is_integer() ? r.integer : 0;
}

int64_t RedisClient::rpush(const std::string& key, const std::string& value) {
    auto r = execute({"RPUSH", key, value});
    return r.is_integer() ? r.integer : 0;
}

std::string RedisClient::lpop(const std::string& key) {
    auto r = execute({"LPOP", key});
    return r.is_string() ? r.str : "";
}

std::string RedisClient::rpop(const std::string& key) {
    auto r = execute({"RPOP", key});
    return r.is_string() ? r.str : "";
}

int64_t RedisClient::llen(const std::string& key) {
    auto r = execute({"LLEN", key});
    return r.is_integer() ? r.integer : 0;
}

std::vector<std::string> RedisClient::lrange(const std::string& key, int64_t start, int64_t stop) {
    std::vector<std::string> result;
    auto r = execute({"LRANGE", key, std::to_string(start), std::to_string(stop)});
    if (r.is_array()) {
        for (const auto& e : r.elements) result.push_back(e.str);
    }
    return result;
}

int RedisClient::sadd(const std::string& key, const std::string& member) {
    auto r = execute({"SADD", key, member});
    return r.is_integer() ? (int)r.integer : 0;
}

int RedisClient::srem(const std::string& key, const std::string& member) {
    auto r = execute({"SREM", key, member});
    return r.is_integer() ? (int)r.integer : 0;
}

bool RedisClient::sismember(const std::string& key, const std::string& member) {
    auto r = execute({"SISMEMBER", key, member});
    return r.is_integer() && r.integer == 1;
}

int64_t RedisClient::scard(const std::string& key) {
    auto r = execute({"SCARD", key});
    return r.is_integer() ? r.integer : 0;
}

std::vector<std::string> RedisClient::smembers(const std::string& key) {
    std::vector<std::string> result;
    auto r = execute({"SMEMBERS", key});
    if (r.is_array()) {
        for (const auto& e : r.elements) result.push_back(e.str);
    }
    return result;
}

int RedisClient::zadd(const std::string& key, double score, const std::string& member) {
    auto r = execute({"ZADD", key, std::to_string(score), member});
    return r.is_integer() ? (int)r.integer : 0;
}

int RedisClient::zrem(const std::string& key, const std::string& member) {
    auto r = execute({"ZREM", key, member});
    return r.is_integer() ? (int)r.integer : 0;
}

double RedisClient::zscore(const std::string& key, const std::string& member) {
    auto r = execute({"ZSCORE", key, member});
    return r.is_string() ? std::stod(r.str) : 0;
}

int64_t RedisClient::zrank(const std::string& key, const std::string& member) {
    auto r = execute({"ZRANK", key, member});
    return r.is_integer() ? r.integer : -1;
}

int64_t RedisClient::zcard(const std::string& key) {
    auto r = execute({"ZCARD", key});
    return r.is_integer() ? r.integer : 0;
}

std::vector<std::string> RedisClient::zrange(const std::string& key, int64_t start, int64_t stop) {
    std::vector<std::string> result;
    auto r = execute({"ZRANGE", key, std::to_string(start), std::to_string(stop)});
    if (r.is_array()) {
        for (const auto& e : r.elements) result.push_back(e.str);
    }
    return result;
}

} // namespace redis
} // namespace chwell