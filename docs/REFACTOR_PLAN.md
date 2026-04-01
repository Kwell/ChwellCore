# ChwellCore 配置文件驱动改造计划

## 1. 配置文件扩展

### 1.1 新的 server.conf 格式

```conf
# 服务配置
server:
  name: "game_server"
  bus_id: "8.8.8.1"
  listen_port: 9000
  worker_threads: 4
  
# 组件配置
components:
  - name: "ProtocolRouter"
    enabled: true
    priority: 1
    
  - name: "SessionManager"
    enabled: true
    priority: 2
    
  - name: "PlayerManager"
    enabled: true
    priority: 3
    
# 日志配置
log:
  level: "INFO"
  path: "./logs/"
  
# 网络配置
network:
  max_connections: 10000
  heartbeat_interval: 30
```

### 1.2 环境差异化配置

```bash
# 开发环境
./bin/server -c conf/server.conf -e dev

# 测试环境
./bin/server -c conf/server.conf -e test

# 生产环境
./bin/server -c conf/server.conf -e prod
```

---

## 2. Component 接口扩展

### 2.1 新增生命周期阶段

```cpp
class Component {
public:
    virtual ~Component() = default;
    
    // 组件名称
    virtual std::string name() const = 0;
    
    // 阶段1: 初始化（组件内部初始化）
    virtual bool Init() { return true; }
    
    // 阶段2: 后初始化（依赖注入完成）
    virtual bool PostInit() { return true; }
    
    // 阶段3: 检查配置
    virtual bool CheckConfig() { return true; }
    
    // 阶段4: 更新前准备
    virtual bool PreUpdate() { return true; }
    
    // 阶段5: 主更新循环
    virtual bool Update(int64_t delta_ms) { return true; }
    
    // 阶段6: 关闭前清理
    virtual bool PreShut() { return true; }
    
    // 阶段7: 关闭
    virtual bool Shut() { return true; }
    
    // 消息回调
    virtual void on_message(const net::TcpConnectionPtr& conn, std::string_view data) {}
    virtual void on_disconnect(const net::TcpConnectionPtr& conn) {}
};
```

### 2.2 Service 生命周期管理

```cpp
class Service {
public:
    // 按顺序调用各阶段
    bool Init() {
        for (auto& comp : components_) {
            if (!comp->Init()) return false;
        }
        return true;
    }
    
    bool PostInit() {
        for (auto& comp : components_) {
            if (!comp->PostInit()) return false;
        }
        return true;
    }
    
    // ... 其他阶段
    
    // 主循环
    void Update(int64_t delta_ms) {
        for (auto& comp : components_) {
            comp->Update(delta_ms);
        }
    }
};
```

---

## 3. 插件层概念

### 3.1 IService 接口

```cpp
class IService {
public:
    virtual ~IService() = default;
    
    virtual const std::string& GetName() const = 0;
    virtual bool Install() = 0;     // 注册组件
    virtual bool Uninstall() = 0;   // 注销组件
};
```

---

## 4. 改造文件清单

| 文件 | 改造内容 |
|------|----------|
| `config/server.conf` | 扩展配置格式，添加组件配置 |
| `include/chwell/service/component.h` | 添加 7 阶段生命周期 |
| `include/chwell/service/service.h` | 实现生命周期管理 |
| `include/chwell/service/plugin.h` | 新增插件接口 |
| `src/service/service.cpp` | 实现生命周期逻辑 |

---

**优先级**: 1 → 2 → 3
**开始时间**: 2026-04-01 18:52
