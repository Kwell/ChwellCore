#include <gtest/gtest.h>

#include "chwell/pool/object_pool.h"

using namespace chwell;

struct TestObject {
    int id;
    std::string name;
    
    TestObject() : id(0), name("") {}
    void reset() {
        id = 0;
        name = "";
    }
};

TEST(ObjectPoolTest, AcquireAndAutoRelease) {
    pool::ObjectPool<TestObject> pool;
    
    {
        auto obj = pool.acquire();
        EXPECT_NE(obj, nullptr);
        EXPECT_EQ(obj->id, 0);
        EXPECT_EQ(obj->name, "");
        
        obj->id = 42;
        obj->name = "test";
    }
    
    // 对象自动归还
    EXPECT_EQ(pool.borrowed_count(), 0);
}

TEST(ObjectPoolTest, AcquireRawAndRelease) {
    pool::ObjectPool<TestObject> pool;
    
    auto obj = pool.acquire_raw();
    EXPECT_NE(obj, nullptr);
    EXPECT_EQ(pool.borrowed_count(), 1);
    
    obj->id = 100;
    obj->name = "raw";
    
    pool.release(obj);
    EXPECT_EQ(pool.borrowed_count(), 0);
}

TEST(ObjectPoolTest, PoolCapacity) {
    pool::ObjectPoolConfig<TestObject> config;
    config.initial_size = 2;
    config.max_size = 3;
    
    pool::ObjectPool<TestObject> pool(config);
    
    EXPECT_EQ(pool.created_count(), 2);
    EXPECT_EQ(pool.available_count(), 2);
    
    auto obj1 = pool.acquire_raw();
    auto obj2 = pool.acquire_raw();
    auto obj3 = pool.acquire_raw();
    
    EXPECT_NE(obj1, nullptr);
    EXPECT_NE(obj2, nullptr);
    EXPECT_NE(obj3, nullptr);
    EXPECT_EQ(pool.created_count(), 3);
    
    // Pool exhausted
    auto obj4 = pool.acquire_raw();
    EXPECT_EQ(obj4, nullptr);
    
    pool.release(obj1);
    pool.release(obj2);
    pool.release(obj3);
}

TEST(ObjectPoolTest, ObjectReset) {
    pool::ObjectPoolConfig<TestObject> config;
    config.initial_size = 1;
    
    pool::ObjectFactory<TestObject> factory;
    factory.reset = [](TestObject* obj) {
        obj->reset();
    };
    
    pool::ObjectPool<TestObject> pool(config, factory);
    
    auto obj1 = pool.acquire();
    obj1->id = 99;
    obj1->name = "modified";
    
    // Release and get another
    obj1.reset();  // Auto-release
    
    auto obj2 = pool.acquire();
    EXPECT_EQ(obj2->id, 0);  // Should be reset
    EXPECT_EQ(obj2->name, "");
}

TEST(ObjectPoolTest, AvailableCount) {
    pool::ObjectPoolConfig<TestObject> config;
    config.initial_size = 5;
    
    pool::ObjectPool<TestObject> pool(config);
    
    EXPECT_EQ(pool.available_count(), 5);
    
    auto obj1 = pool.acquire();
    EXPECT_EQ(pool.available_count(), 4);
    
    auto obj2 = pool.acquire();
    EXPECT_EQ(pool.available_count(), 3);
    
    obj1.reset();
    EXPECT_EQ(pool.available_count(), 4);
    
    obj2.reset();
    EXPECT_EQ(pool.available_count(), 5);
}

TEST(ObjectPoolTest, Expand) {
    pool::ObjectPoolConfig<TestObject> config;
    config.initial_size = 2;
    config.max_size = 10;
    
    pool::ObjectPool<TestObject> pool(config);
    
    EXPECT_EQ(pool.created_count(), 2);
    
    pool.expand(3);
    EXPECT_EQ(pool.created_count(), 5);
    EXPECT_EQ(pool.available_count(), 5);
}

TEST(ObjectPoolTest, Shrink) {
    pool::ObjectPoolConfig<TestObject> config;
    config.initial_size = 10;
    config.max_size = 10;
    
    pool::ObjectPool<TestObject> pool(config);
    
    EXPECT_EQ(pool.available_count(), 10);
    
    pool.shrink(5);
    EXPECT_EQ(pool.available_count(), 5);
}

TEST(BufferPoolTest, AcquireAndRelease) {
    pool::BufferPool pool(1024, 5, 10);
    
    auto buf = pool.acquire();
    EXPECT_NE(buf, nullptr);
    EXPECT_EQ(buf->size(), 0);
    EXPECT_GE(buf->capacity(), 1024);
    
    buf->resize(100);
    EXPECT_EQ(buf->size(), 100);
    
    pool.release(std::move(buf));
    EXPECT_EQ(pool.available_count(), 5);
}

TEST(BufferPoolTest, PoolExhaustion) {
    pool::BufferPool pool(1024, 2, 2);
    
    auto buf1 = pool.acquire();
    auto buf2 = pool.acquire();
    
    EXPECT_EQ(pool.available_count(), 0);
    
    auto buf3 = pool.acquire();
    EXPECT_EQ(buf3, nullptr);  // Pool exhausted
}

TEST(GlobalBufferPoolTest, AcquireAndRelease) {
    auto buf = pool::GlobalBufferPool::acquire(4096);
    EXPECT_NE(buf, nullptr);
    
    buf->resize(100);
    EXPECT_EQ(buf->size(), 100);
    
    pool::GlobalBufferPool::release(std::move(buf), 4096);
}
