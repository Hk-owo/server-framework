//
// Created by lacas on 2026/3/10.
//

#include "Server/HttpServer.h"
#include "Http/HttpParser.h"
#include "Http/HttpResponse.h"
#include "Logger.h"

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace std;

// ─────────────────────────────────────────────
// 工具函数
// ─────────────────────────────────────────────

static std::string getParam(const HttpRequest& req, const std::string& key) {
    return req.get_param_value(key);
}

static std::string getHeader(const HttpRequest& req, const std::string& key) {
    auto it = req.headers.find(key);
    if (it != req.headers.end()) return it->second;

    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    it = req.headers.find(lower);
    return (it != req.headers.end()) ? it->second : "";
}

// ─────────────────────────────────────────────
// 构造 / 路由注册
// ─────────────────────────────────────────────

// 实例数：默认取 CPU 核数，可用环境变量 SF_LOOPS 覆盖（便于对比不同实例数）
static unsigned resolveLoopCount() {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 1;
    if (const char* env = std::getenv("SF_LOOPS")) {
        const int v = std::atoi(env);
        if (v > 0) n = static_cast<unsigned>(v);
    }
    return n;
}

HttpServer::HttpServer() {
    LOGGER_INF("HttpServer: constructor start");

    const unsigned loops = resolveLoopCount();
    mServers.reserve(loops);
    for (unsigned i = 0; i < loops; ++i) {
        auto srv = std::make_shared<Server>();
        registerRoutes(*srv);                          // 每实例一份只读路由表
        // SO_REUSEPORT：多个实例共享同一端口，内核按连接 4 元组哈希分发
        if (srv->listen("0.0.0.0", "8080") != 0) {
            LOGGER_ERROR("HttpServer: listen failed on event loop {}", i);
            exit(1);
        }
        mServers.push_back(std::move(srv));
    }

    mLoops.reserve(loops);
    for (auto& srv : mServers)
        mLoops.emplace_back([srv] { srv->run(); });

    LOGGER_INF("HttpServer: {} event loop(s) running on 0.0.0.0:8080", loops);
    for (auto& t : mLoops)
        t.join();            // 阻塞（与原设计一致：构造函数不返回）
}

void HttpServer::registerRoutes(Server& server) {
    // GET /hello?name=xxx
    server.Get("/hello", [](const HttpRequest& req, HttpResponse& res) {
        auto name = getParam(req, "name");
        res.set_status(200)
           .set_header("Content-Type", "text/plain")
           .set_body("Hello, " + (name.empty() ? "World" : name));
    });

    // POST /echo
    server.Post("/echo", [](const HttpRequest& req, HttpResponse& res) {
        res.set_status(200)
           .set_header("Content-Type", "text/plain")
           .set_body(req.body);
    });

    // GET /json
    server.Get("/json", [](const HttpRequest& req, HttpResponse& res) {
        (void)req;
        res.set_status(200)
           .set_json(R"({"status":"ok","framework":"io_uring + c++23 coroutines"})");
    });

    // GET /api/status
    server.Get("/api/status", [](const HttpRequest& req, HttpResponse& res) {
        (void)req;
        res.set_status(200)
           .set_json(R"({"success":true,"data":{"uptime":"running","version":"1.0.0"}})");
    });

    // GET /async/status — 异步 handler 示例（GetAsync 注册，投全局线程池执行）
    // 模拟耗时任务：sleep 1ms 后返回，不阻塞 io_uring 事件循环
    server.GetAsync("/async/status", [](const HttpRequest& req, HttpResponse& res) {
        (void)req;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        res.set_status(200)
           .set_json(R"({"success":true,"data":{"mode":"async","threadpool":"global"}})");
    });

    // GET /api/user/{id} — 前缀匹配示例
    server.Get("/api/user/", [](const HttpRequest& req, HttpResponse& res) {
        const std::string& path = req.parsed_uri().path;
        const std::string prefix = "/api/user/";
        std::string userId = path.substr(prefix.size());

        if (userId.empty()) {
            res.set_status(400)
               .set_json(R"({"success":false,"msg":"user id required"})");
            return;
        }

        res.set_status(200)
           .set_json("{\"success\":true,\"data\":{\"userId\":\"" + userId + "\"}}");
    });
}
