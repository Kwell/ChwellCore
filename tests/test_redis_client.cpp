#include <gtest/gtest.h>

#include "chwell/redis/redis_client.h"

using namespace chwell;

class RedisClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        redis::RedisConfig config;
        client_ = std::make_unique<redis::RedisClient>(config);
        client_->connect();
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