//
// Created by lacas on 2026/3/10.
//

#include "Server/HttpServer.h"
#include "Logger.h"

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

HttpServer::HttpServer() {
    LOGGER_INF("HttpServer: constructor start");
    registerRoutes();
    LOGGER_INF("HttpServer: starting server on 0.0.0.0:8080");
    server.listen("0.0.0.0", "8080");
    LOGGER_INF("HttpServer: running server...");
    server.run();
}

void HttpServer::registerRoutes() {
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
