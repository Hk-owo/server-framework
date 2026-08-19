
# 基于 Linux io_uring 与 C++23 协程的高性能 HTTP 服务器框架

---

## 目录

- [项目特性](#项目特性)
- [快速开始](#快速开始)
- [系统架构](#系统架构)
- [核心模块](#核心模块)
- [编译构建](#编译构建)
- [API 参考](#api-参考)
- [设计亮点](#设计亮点)
- [目录结构](#目录结构)

---

## 项目特性

- **io_uring 全链路异步**

  Accept / Recv / Send / Close / Poll 均通过 io_uring 提交，减少系统调用与线程切换开销。支持 **Multishot Accept**，一次提交即可持续接收新连接。

- **C++23 协程连接模型**

  每个 TCP 连接对应一个独立协程，通过 `co_yield` 挂起 IO、`co_await` 组合异步逻辑，代码风格接近同步写法，无需维护复杂的状态机。

- **同步快速路径 + 显式异步**

  HTTP Handler 默认在事件循环线程**同步执行**（零调度开销，适合快任务）；耗时 handler 通过 `GetAsync/PostAsync` 显式注册为异步，经 `QueryAwaiter` 直投全局线程池执行，避免阻塞 io_uring 事件循环。支持 `co_await` 等待后台任务完成后安全恢复协程。

- **eventfd 跨线程协程去重恢复**

  后台线程完成任务后通过 `eventfd` 唤醒主循环；`compare_exchange_strong` 保证同一协程不会被重复入队，防止竞态。

- **批量 CQE 消费**

  事件循环使用 `io_uring_for_each_cqe` + `io_uring_cq_advance` 批量处理完成事件，显著减少内核态切换。

- **双实例共享队列线程池**

  全局任务池（8 个工作线程）承载业务任务，时间轮专用池（2 个工作线程）只执行延迟 / 超时等轻量任务，互不干扰。所有 worker 共享一个任务队列，**谁空闲谁取**——任何线程被慢任务拖住都不会导致排队任务无人处理，也不会让其他线程空转。enqueue 无锁、dequeue 消费者互斥，关键变量使用 `alignas(64)` 消除伪共享。

- **四级级联时间轮**

  毫秒级精度的 `ms -> sec -> min -> hour` 级联时间轮，用于延迟任务调度与协程超时控制。由 io_uring 1ms 超时驱动，支持追赶机制。

- **HTTP/1.1 协议支持**

  完整支持请求行与 Header 解析、URI Query 自动解码、`Content-Length` 与 `Chunked Transfer-Encoding`、Keep-Alive 长连接。

- **短写自动续传**

  Send 未完全发送时自动记录 `write_offset`，下次从断点继续发送，上层业务无感知。

- **异步日志**

  基于 `spdlog` 的异步 Logger，支持控制台彩色输出与按大小轮转的日志文件，提供 Trace / Debug / Info / Warn / Error / Critical 六级日志。

- **分层数据层**

  MySQL 连接池 + Redis 连接池，Cache-Aside 缓存策略，QueryResult&lt;T&gt; 统一结果包装。

---

## 快速开始

### 环境要求

| 项目 | 版本要求 |
|------|----------|
| 操作系统 | Linux 内核 >= 5.10（推荐 >= 6.0 以完整支持 multishot accept） |
| 编译器 | 支持 C++23 的 GCC 或 Clang |
| 依赖库 | `liburing`、`spdlog`、`fmt`、`nlohmann-json` |

### 最小示例

```cpp
#include "Server/Server.h"
#include "Logger.h"

int main() {
    Logger::init("Demo", "../logs/demo.log");

    Server srv;

    srv.Get("/hello", [](const HttpRequest& req, HttpResponse& res) {
        auto name = req.get_param_value("name");
        res.set_status(200)
           .set_body("Hello, " + (name.empty() ? "World" : name));
    });

    srv.Post("/echo", [](const HttpRequest& req, HttpResponse& res) {
        res.set_status(200).set_body(req.body);
    });

    if (srv.listen("0.0.0.0", "8080") != 0) return 1;
    srv.run();  // 进入阻塞事件循环
    return 0;
}
```

### 业务层快速启动（HttpServer）

```cpp
#include "Server/HttpServer.h"

int main() {
    HttpServer httpServer;  // 自动注册路由、启动监听、进入事件循环
    return 0;
}
```

`HttpServer` 内置完整的 Dashboard REST API（登录鉴权、订单管理、会员查询等），基于 Redis Token 鉴权与 MySQL 数据层。

### 编译运行

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 测试验证

```bash
curl "http://localhost:8080/hello?name=GitHub"
curl -X POST "http://localhost:8080/echo" -d "ping"
```

---

## 系统架构

```
┌─────────────────────────────────────────────┐
│          单线程 io_uring 事件循环            │
│   io_uring_for_each_cqe → handleCqe         │
│   io_uring_cq_advance → 批量标记已消费       │
│   1ms 超时唤醒 → 驱动 TimeWheel              │
└──────────────┬──────────────────────────────┘
               │ CQE 唤醒
    ┌──────────▼──────────┐
    │   每个连接一个协程   │  clientConnect(fd)
    │   co_await READ     │  HttpParser / HttpResponse
    │   co_yield WRITE    │
    │   co_await Query    │  ← 仅异步 handler 走线程池
    │   co_yield CLOSE    │
    └──────┬─────────┬────┘
           │业务任务  │ 延迟/超时回调
    ┌──────▼─────┐  ┌▼──────────────┐
    │ Global     │  │  TimeWheelTop │
    │ ThreadPool │  │  级联推进      │
    │ 8 工作线程 │  └──────┬───────┘
    │ 共享队列   │         │
    └────────────┘  ┌──────▼───────────┐
                   │ TimeWheelThreadPool│
                   │ 2 工作线程        │
                   └──────────────────┘
```

**事件循环**：单线程 io_uring 批量收取所有 CQE，根据 `OpType` 分发到 `handleCqe` 处理。空队列时以 1ms 超时阻塞等待，既降低 CPU 占用，又精确驱动时间轮。  
**协程模型**：每个连接一个 `coro::Task<void>`，通过自定义 `IouringAwaiter` 挂起 IO，`QueryAwaiter` 将业务逻辑异步化。  
**跨线程协程恢复**：后台线程通过 **eventfd + MPSCQueue** 将协程句柄带回主循环；`resumePending` CAS 标志防止重复入队，确保线程安全。

---

## 核心模块

### Server

框架主类，封装 io_uring 生命周期、路由注册、协程调度与 CQE 分发。

| 接口 | 说明 |
|------|------|
| `listen(bind, port)` | 创建 socket、绑定地址、开始监听 |
| `run(entries, flags)` | 初始化 io_uring 并进入事件循环 |
| `Get(pattern, handler)` | 注册 GET 路由（支持 `/prefix/` 前缀匹配） |
| `Post(pattern, handler)` | 注册 POST 路由 |
| `handleCqe(ctx, res, cflags)` | 独立 CQE 处理函数，解耦事件循环与业务逻辑 |

### HttpParser

流式 HTTP/1.1 请求解析器。

```cpp
HttpParser parser;
parser.feed(buffer, n);
if (auto req = parser.try_parse()) {
    // req->method, req->uri, req->headers, req->body
    auto param = req->get_param_value("key");
}
```

- `feed()` 追加网络数据，`try_parse()` 在报文完整时返回 `HttpRequest`
- 支持 `Content-Length` 与 `Chunked Transfer-Encoding`
- URI Query 自动 URL decode，Header key 统一小写存储

### HttpResponse

流式 HTTP 响应构造器。

```cpp
HttpResponse res;
res.set_status(200)
   .set_header("X-Custom", "value")
   .set_json(R"({"status":"ok"})");
std::string msg = res.build();   // 生成完整 HTTP 报文
```

### coro::Task&lt;T&gt;

轻量级协程封装（`include/Server/CoroTask.h`），提供 `T` 的泛化版本与 `void` 特化版本。

- `co_yield` 自定义 Awaiter 挂起 IO 操作
- `co_await` 组合异步逻辑（如 `QueryAwaiter` 异步投递线程池）
- `resume()` / `done()` / `get()` 控制协程生命周期
- `Task<void>::raw_handle()` 获取原始 `std::coroutine_handle<>`，用于外部存储与恢复

### QueryAwaiter

将同步 HTTP Handler 异步化的核心组件。

```cpp
co_await QueryAwaiter{server, connState, [&]() {
    handler(req, res);  // 在全局线程池后台执行
}};
```

- 利用 `compare_exchange_strong` 设置 `resumePending` 标志，防止同一协程被重复入队
- 业务任务直投全局线程池（8 工作线程）异步执行，经 eventfd 唤醒恢复协程，无固定调度延迟

### ThreadPool

线程数可配置的共享队列线程池（N 个工作线程共享一个任务队列），框架内置两个独立实例：

- `ThreadPool::global_instance()`：**全局任务池**，8 个工作线程，承载业务 handler 等通用任务
- `ThreadPool::timewheel_instance()`：**时间轮专用池**，2 个工作线程，只执行时间轮到期的轻量任务

调度模型：

- **共享任务队列**：所有 worker 从同一队列取任务，谁空闲谁取，任务不绑定线程——任何线程被慢任务拖住时，排队任务由其他空闲 worker 继续完成，不会饿死或空转
- **enqueue 无锁**（MPSC 多生产者安全），**dequeue 消费者互斥**（保证节点回收安全，任务在锁外执行）
- **广播唤醒**：`submit` 入队后唤醒所有空闲 worker 的等待信号，空闲线程全部醒来竞争取任务

### TimeWheelTop

四级级联时间轮。

```cpp
TimeWheelTop tw;
tw.add_task([]{ /* 延迟任务 */ }, {0, 0, 0, 3});  // 延迟 3ms
```

- 内部线程以 io_uring 1ms 超时驱动推进
- 支持追赶机制：若线程滞后，会连续推进直到追上理论时刻
- 到期任务自动投递到时间轮专用线程池执行
- 延迟小于 3ms 的任务直接投递时间轮专用线程池，避免时间轮精度抖动

### WaitQueue

无锁队列家族，底层通过 io_uring NOP CQE 实现线程间阻塞与唤醒。

| 类 | 模型 | 容量 | 用途 |
|----|------|------|------|
| `MPSCQueue<T>` | 多生产者-单消费者 | 动态链表 | 线程池 per-worker 唤醒信号、协程恢复队列 |
| `MPSCBase<T>` | 多生产者-多消费者（dequeue 加锁） | 动态链表 | 线程池共享任务队列 |

### Logger

基于 `spdlog` 的异步日志单例。

```cpp
Logger::init("Server", "../logs/app.log", maxSize, maxFiles, Logger::Level::Info);
LOGGER_INF("Server started on {}:{}", host, port);
```

- 异步非阻塞，独立线程池刷盘
- 控制台彩色输出 + 文件按大小轮转切割

---

## 编译构建

```bash
# 安装依赖（以 Debian/Ubuntu 为例）
sudo apt install liburing-dev libspdlog-dev nlohmann-json3-dev

# 构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

CMake 选项：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CMAKE_BUILD_TYPE` | `Release` | `Release` / `Debug` |

---

## API 参考

### HttpRequest

| 成员 / 方法 | 说明 |
|-------------|------|
| `method`, `uri`, `version` | 请求行字段 |
| `headers` | Header 字典（key 已转小写） |
| `body` | 请求体内容 |
| `parsed_uri()` | 返回 `ParsedUri`，含 `path` 与 `query`（懒解析，结果缓存） |
| `get_header_value(name)` | 大小写不敏感查找 Header |
| `get_param_value(key)` | 获取 URL Query 参数（已 URL decode） |

### HttpResponse

| 方法 | 说明 |
|------|------|
| `set_status(code)` / `set_status(code, reason)` | 设置状态码 |
| `set_header(name, value)` | 设置响应头 |
| `set_body(body, content_type)` | 设置响应体（自动补全 Content-Length） |
| `set_json(json_body)` | 快捷设置 JSON 响应（Content-Type: application/json） |
| `build()` | 序列化为完整 HTTP/1.1 响应字符串 |

---

## 设计亮点

1. **协程 + io_uring 零拷贝思维**

   每个连接是独立协程，IO 操作通过 `co_yield IouringAwaiter` 挂起，CQE 到达后恢复。无需为每个连接维护复杂的状态机，代码接近同步风格。

2. **同步快速路径 + QueryAwaiter 显式异步**

   快 handler（字符串拼接、本地查询等）默认在事件循环线程同步执行，无调度开销；慢 handler 通过 `GetAsync/PostAsync` 显式注册，由 `QueryAwaiter` 投递到全局线程池，协程 `co_await` 等待完成后自动恢复，避免阻塞 CQE 消费。

3. **eventfd 跨线程协程去重恢复**

   当后台线程完成任务后，通过 `eventfd` 写入 + `io_uring_poll_add` 监听，将恢复事件带回 io_uring 主循环。`resumePending` 原子标志 + `compare_exchange_strong` 保证同一协程不会被重复入队，彻底消除竞态。

4. **批量 CQE 消费**

   使用 `io_uring_for_each_cqe` 遍历整个完成队列，`io_uring_cq_advance` 一次性标记已消费，相比逐个 `cqe_seen` 显著减少内核态切换与内存屏障开销。

5. **1ms 超时驱动时间轮**

   事件循环在 CQE 为空时以 1ms 超时阻塞等待，既保证空载时 CPU 占用趋近于零，又精确驱动时间轮推进，无需独立定时线程。

6. **共享队列与阶梯式等待**

   工作线程从共享任务队列取任务，空队列时通过独立 io_uring 实例做 `peek -> wait` 的阶梯式阻塞，兼顾低延迟与零空闲 CPU 占用。

7. **广播唤醒**

   `submit` 入队后广播唤醒所有空闲 worker 的等待信号，空闲线程全部醒来竞争取任务；任何线程被慢任务拖住时，其余 worker 继续消化共享队列，不会空转。

8. **时间轮级联与短延迟兜底**

   当延迟小于 3ms 时，任务直接投递线程池执行，避免进入时间轮产生精度抖动；级联设计保证从毫秒到小时的延迟都能以 O(1) 插入。

---

## 目录结构

```
include/
├── Server/
│   ├── Server.h          # io_uring HTTP 服务器主类
│   ├── Router.h          # 路由表（GET/POST 注册与匹配）
│   ├── HttpServer.h      # 业务层 Dashboard HTTP 服务器
│   └── CoroTask.h        # C++23 协程 Task<T>
├── Http/
│   ├── HttpParser.h      # HTTP/1.1 请求解析器
│   └── HttpResponse.h    # HTTP 响应构造器
├── WaitQueue/
│   ├── SPSCBase.h / SPSCQueue.h   # 无锁 SPSC 队列 + uring 唤醒
│   └── MPSCBase.h / MPSCQueue.h   # 无锁 MPSC 队列 + uring 唤醒
├── ThreadPool.h          # 共享队列线程池
├── TimeWheel.h           # 四级时间轮
├── Logger.h              # spdlog 异步日志封装
└── DataBaseQuery/        # 数据层（连接池、查询封装、缓存管理）
src/
├── Http/
│   ├── HttpParser.cpp    # 解析器实现
│   └── HttpResponse.cpp  # 响应构造实现
├── HttpServer/
│   ├── Server.cpp        # Server 实现（含 ConnCtx / awaiter 实现）
│   ├── Router.cpp        # 路由匹配实现
│   └── Httpserver.cpp    # HttpServer 业务层实现
├── ThreadPool/
│   └── ThreadPool.cpp
├── TimeWheel/
│   └── TimeWheel.cpp
├── Logger/
│   └── Logger.cpp
├── DataBaseQuery/        # MySQL/Redis 连接池与查询实现
└── main.cpp              # 入口：构造 HttpServer
tests/
├── test_connection_pool.cpp
└── test_pool_simple.cpp
```

---

## 许可

本项目为学习/实验性质的高性能服务器框架，可自由修改与扩展。
