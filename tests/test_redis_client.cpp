#include <gtest/gtest.h>

#include "chwell/redis/redis_client.h"
#include <thread>
#include <chrono>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>

using namespace chwell;

namespace {
class RedisEnvironment {
public:
    explicit RedisEnvironment(const char* value = nullptr) {
        const char* previous = std::getenv("CHWELL_REDIS_MOCK");
        existed_ = previous != nullptr;
        if (previous) previous_ = previous;
        assign(value);
    }
    ~RedisEnvironment() { assign(existed_ ? previous_.c_str() : nullptr); }
private:
    static void assign(const char* value) {
        if (value) setenv("CHWELL_REDIS_MOCK", value, 1);
        else unsetenv("CHWELL_REDIS_MOCK");
    }
    bool existed_;
    std::string previous_;
};

class RedisTestSocket {
public:
    RedisTestSocket() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            socklen_t size = sizeof(address);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0) {
                port = ntohs(address.sin_port);
            }
        }
    }
    ~RedisTestSocket() { if (fd >= 0) ::close(fd); }
    int accept_peer() const {
        pollfd pending{fd, POLLIN, 0};
        if (::poll(&pending, 1, 2000) <= 0 || !(pending.revents & POLLIN)) return -1;
        return ::accept(fd, nullptr, nullptr);
    }
    int fd = -1;
    int port = 0;
};
} // namespace

TEST(RedisConnectionTest, RefusedConnectionNeverFallsBackToMemory) {
    RedisEnvironment environment;
    RedisTestSocket socket; // Bound but not listening: deterministic refusal.
    ASSERT_GT(socket.port, 0);
    redis::RedisConfig config;
    config.port = socket.port;
    config.connection_timeout_ms = 100;
    redis::RedisClient client(config);
    EXPECT_FALSE(client.connect());
    EXPECT_FALSE(client.is_connected());
    EXPECT_FALSE(client.is_mock());
    EXPECT_FALSE(client.set("key", "value"));
    EXPECT_TRUE(client.execute({"GET", "key"}).is_error());
    EXPECT_FALSE(client.set_nx_ex("lock", "token", 10));
    redis::RedisCacheComponent cache(config);
    EXPECT_FALSE(cache.Init());
    EXPECT_TRUE(cache.Shut());
}

TEST(RedisConnectionTest, ExplicitMockAndDisconnectAreEnforcedForAtomicCommands) {
    RedisEnvironment environment;
    redis::RedisConfig config;
    config.mock_mode = true;
    redis::RedisClient client(config);
    EXPECT_FALSE(client.set("key", "value"));
    ASSERT_TRUE(client.connect());
    EXPECT_TRUE(client.is_mock());
    ASSERT_TRUE(client.set_nx_ex("lock", "token", 10));
    EXPECT_FALSE(client.set_nx_ex("invalid", "token", 0));
    client.disconnect();
    EXPECT_FALSE(client.set_nx_ex("other", "token", 10));
    EXPECT_FALSE(client.compare_and_del("lock", "token"));
    EXPECT_FALSE(client.compare_and_expire("lock", "token", 10));
    EXPECT_FALSE(client.set("key", "value"));
}

TEST(RedisConnectionTest, EnvironmentMockMustBeExactlyOne) {
    RedisTestSocket socket;
    ASSERT_GT(socket.port, 0);
    redis::RedisConfig config;
    config.port = socket.port;
    {
        RedisEnvironment environment("1");
        redis::RedisClient client(config);
        ASSERT_TRUE(client.connect());
        EXPECT_TRUE(client.is_mock());
    }
    {
        RedisEnvironment environment("1garbage");
        redis::RedisClient client(config);
        EXPECT_FALSE(client.connect());
        EXPECT_FALSE(client.is_mock());
    }
}

TEST(RedisConnectionTest, AuthenticationAndDatabaseRejectionsFailConnection) {
    RedisEnvironment environment;
    for (bool auth : {true, false}) {
        RedisTestSocket socket;
        ASSERT_GT(socket.port, 0);
        ASSERT_EQ(::listen(socket.fd, 1), 0);
        std::string command;
        std::thread peer([&] {
            int fd = socket.accept_peer();
            if (fd < 0) return;
            char buffer[1024];
            pollfd pending{fd, POLLIN, 0};
            ssize_t count = ::poll(&pending, 1, 2000) > 0
                ? ::recv(fd, buffer, sizeof(buffer), 0) : -1;
            if (count > 0) command.assign(buffer, static_cast<std::size_t>(count));
            const std::string rejection = "-ERR test rejection\r\n";
            ::send(fd, rejection.data(), rejection.size(), MSG_NOSIGNAL);
            ::close(fd);
        });
        redis::RedisConfig config;
        config.port = socket.port;
        config.read_timeout_ms = 500;
        if (auth) config.password = "test-password";
        else config.db = 3;
        redis::RedisClient client(config);
        EXPECT_FALSE(client.connect());
        EXPECT_FALSE(client.is_connected());
        EXPECT_FALSE(client.is_mock());
        peer.join();
        EXPECT_NE(command.find(auth ? "AUTH" : "SELECT"), std::string::npos);
    }
}

TEST(RedisConnectionTest, ClosedPeerInvalidatesConnection) {
    RedisEnvironment environment;
    RedisTestSocket socket;
    ASSERT_GT(socket.port, 0);
    ASSERT_EQ(::listen(socket.fd, 1), 0);
    std::thread peer([&] {
        int fd = socket.accept_peer();
        if (fd < 0) return;
        char buffer[128];
        pollfd pending{fd, POLLIN, 0};
        if (::poll(&pending, 1, 2000) > 0) ::recv(fd, buffer, sizeof(buffer), 0);
        ::close(fd);
    });
    redis::RedisConfig config;
    config.port = socket.port;
    config.read_timeout_ms = 500;
    redis::RedisClient client(config);
    EXPECT_TRUE(client.connect());
    EXPECT_TRUE(client.execute({"PING"}).is_error());
    EXPECT_FALSE(client.is_connected());
    EXPECT_FALSE(client.is_mock());
    peer.join();
}

class RedisClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        redis::RedisConfig config;
        config.mock_mode = true;
        client_ = std::make_unique<redis::RedisClient>(config);
        ASSERT_TRUE(client_->connect());
        ASSERT_TRUE(client_->is_mock());
    }
    
    void TearDown() override {
        client_->disconnect();
    }
    
    std::unique_ptr<redis::RedisClient> client_;
};

// String operations
TEST_F(RedisClientTest, SetAndGet) {
    EXPECT_TRUE(client_->set("key1", "value1"));
    
    std::string value;
    EXPECT_TRUE(client_->get("key1", value));
    EXPECT_EQ(value, "value1");
}

TEST_F(RedisClientTest, GetNonExistent) {
    std::string value;
    EXPECT_FALSE(client_->get("nonexistent_key", value));
}

TEST_F(RedisClientTest, SetEx) {
    EXPECT_TRUE(client_->setex("temp_key", 10, "temp_value"));
    
    std::string value;
    EXPECT_TRUE(client_->get("temp_key", value));
    EXPECT_EQ(value, "temp_value");
}

TEST_F(RedisClientTest, SetNx) {
    EXPECT_TRUE(client_->setnx("nx_key", "first"));
    EXPECT_FALSE(client_->setnx("nx_key", "second"));
    
    std::string value;
    client_->get("nx_key", value);
    EXPECT_EQ(value, "first");
}

TEST_F(RedisClientTest, Del) {
    client_->set("del_key", "value");
    EXPECT_EQ(client_->del("del_key"), 1);
    EXPECT_EQ(client_->del("del_key"), 0);
}

TEST_F(RedisClientTest, Exists) {
    EXPECT_FALSE(client_->exists("exists_key"));
    client_->set("exists_key", "value");
    EXPECT_TRUE(client_->exists("exists_key"));
}

TEST_F(RedisClientTest, Incr) {
    client_->set("counter", "0");
    EXPECT_EQ(client_->incr("counter"), 1);
    EXPECT_EQ(client_->incr("counter"), 2);
    EXPECT_EQ(client_->incrby("counter", 10), 12);
}

// Hash operations
TEST_F(RedisClientTest, HSetHGet) {
    EXPECT_TRUE(client_->hset("hash1", "field1", "value1"));
    
    std::string value;
    EXPECT_TRUE(client_->hget("hash1", "field1", value));
    EXPECT_EQ(value, "value1");
}

TEST_F(RedisClientTest, HExists) {
    EXPECT_FALSE(client_->hexists("hash2", "field1"));
    client_->hset("hash2", "field1", "value1");
    EXPECT_TRUE(client_->hexists("hash2", "field1"));
}

TEST_F(RedisClientTest, HDel) {
    client_->hset("hash3", "field1", "value1");
    EXPECT_EQ(client_->hdel("hash3", "field1"), 1);
    EXPECT_EQ(client_->hdel("hash3", "field1"), 0);
}

TEST_F(RedisClientTest, HGetAll) {
    client_->hset("hash4", "f1", "v1");
    client_->hset("hash4", "f2", "v2");
    client_->hset("hash4", "f3", "v3");
    
    auto all = client_->hgetall("hash4");
    EXPECT_EQ(all.size(), 3u);
    EXPECT_EQ(all["f1"], "v1");
    EXPECT_EQ(all["f2"], "v2");
    EXPECT_EQ(all["f3"], "v3");
}

// List operations
TEST_F(RedisClientTest, LPushRPush) {
    EXPECT_EQ(client_->lpush("list1", "first"), 1);
    EXPECT_EQ(client_->rpush("list1", "last"), 2);
    
    auto range = client_->lrange("list1", 0, -1);
    EXPECT_EQ(range.size(), 2u);
    EXPECT_EQ(range[0], "first");
    EXPECT_EQ(range[1], "last");
}

TEST_F(RedisClientTest, LPopRPop) {
    client_->lpush("list2", "a");
    client_->lpush("list2", "b");
    
    EXPECT_EQ(client_->rpop("list2"), "a");
    EXPECT_EQ(client_->lpop("list2"), "b");
    EXPECT_EQ(client_->llen("list2"), 0);
}

// Set operations
TEST_F(RedisClientTest, SAddSRem) {
    EXPECT_EQ(client_->sadd("set1", "member1"), 1);
    EXPECT_EQ(client_->sadd("set1", "member1"), 0);  // Already exists
    
    EXPECT_TRUE(client_->sismember("set1", "member1"));
    EXPECT_EQ(client_->scard("set1"), 1);
    
    EXPECT_EQ(client_->srem("set1", "member1"), 1);
    EXPECT_FALSE(client_->sismember("set1", "member1"));
}

TEST_F(RedisClientTest, SMembers) {
    client_->sadd("set2", "a");
    client_->sadd("set2", "b");
    client_->sadd("set2", "c");
    
    auto members = client_->smembers("set2");
    EXPECT_EQ(members.size(), 3u);
}

// Sorted Set operations
TEST_F(RedisClientTest, ZAddZRem) {
    EXPECT_EQ(client_->zadd("zset1", 100.0, "player1"), 1);
    EXPECT_EQ(client_->zadd("zset1", 200.0, "player2"), 1);
    
    EXPECT_EQ(client_->zscore("zset1", "player1"), 100.0);
    EXPECT_EQ(client_->zrank("zset1", "player2"), 1);
    EXPECT_EQ(client_->zcard("zset1"), 2);
    
    EXPECT_EQ(client_->zrem("zset1", "player1"), 1);
    EXPECT_EQ(client_->zcard("zset1"), 1);
}

TEST_F(RedisClientTest, ZRange) {
    client_->zadd("zset2", 300.0, "c");
    client_->zadd("zset2", 100.0, "a");
    client_->zadd("zset2", 200.0, "b");
    
    auto range = client_->zrange("zset2", 0, -1);
    ASSERT_EQ(range.size(), 3u);
    EXPECT_EQ(range[0], "a");
    EXPECT_EQ(range[1], "b");
    EXPECT_EQ(range[2], "c");
}

// Raw command
TEST_F(RedisClientTest, ExecuteCommand) {
    redis::RedisReply reply = client_->execute({"SET", "cmd_key", "cmd_value"});
    EXPECT_TRUE(reply.ok());
    
    reply = client_->execute({"GET", "cmd_key"});
    EXPECT_TRUE(reply.ok());
    EXPECT_EQ(reply.str, "cmd_value");
}
TEST_F(RedisClientTest, LazyExpireOnHashAndList) {
    client_->hset("exp_hash", "f", "v");
    client_->execute({"EXPIRE", "exp_hash", "1"});
    EXPECT_TRUE(client_->exists("exp_hash"));

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    std::string v;
    EXPECT_FALSE(client_->hget("exp_hash", "f", v));
    EXPECT_FALSE(client_->exists("exp_hash"));

    client_->execute({"RPUSH", "exp_list", "a"});
    client_->execute({"EXPIRE", "exp_list", "1"});
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto r = client_->execute({"LLEN", "exp_list"});
    EXPECT_TRUE(r.is_integer());
    EXPECT_EQ(0, r.integer);
}

TEST_F(RedisClientTest, LazyExpireOnSetAndZset) {
    client_->execute({"SADD", "exp_set", "m"});
    client_->execute({"EXPIRE", "exp_set", "1"});
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto r1 = client_->execute({"SCARD", "exp_set"});
    EXPECT_TRUE(r1.is_integer());
    EXPECT_EQ(0, r1.integer);

    client_->execute({"ZADD", "exp_z", "1", "m"});
    client_->execute({"EXPIRE", "exp_z", "1"});
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto r2 = client_->execute({"ZCARD", "exp_z"});
    EXPECT_TRUE(r2.is_integer());
    EXPECT_EQ(0, r2.integer);
}
