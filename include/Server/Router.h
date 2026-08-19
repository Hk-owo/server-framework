//
// Created by lacas on 2026/8/19.
//

#ifndef WEBPROJECT_ROUTER_H
#define WEBPROJECT_ROUTER_H

#include "Http/HttpParser.h"
#include "Http/HttpResponse.h"
#include <functional>
#include <string>
#include <unordered_map>

// 路由表：GET/POST 注册与匹配（精确匹配 + 目录前缀匹配）。
// 只负责"匹配"，不负责执行——执行策略（同步快速路径 / 异步投递线程池）
// 由调用方根据 Route::async 决定，避免路由组件与调度/IO 层耦合。
class Router {
public:
    using Handler = std::function<void(const HttpRequest& req, HttpResponse& res)>;

    // 一条已注册的路由：handler + 是否异步执行（慢 handler 用）
    struct Route {
        Handler handler;
        bool    async = false;
    };

    // 同步 handler：在事件循环线程直接执行，零调度开销（默认，适合快任务）
    void Get(std::string pattern, Handler handler);
    void Post(std::string pattern, Handler handler);
    // 异步 handler：投递到全局线程池执行，适合耗时任务（数据库/外部调用）
    void GetAsync(std::string pattern, Handler handler);
    void PostAsync(std::string pattern, Handler handler);

    // 查找与请求路径匹配的路由。
    // 命中返回 true 并输出 Route 的拷贝，未命中返回 false。
    bool match(const HttpRequest& req, Route& out) const;

private:
    void add(std::unordered_map<std::string, Route>& table,
             std::string pattern, Handler handler, bool async);

    std::unordered_map<std::string, Route> mGetTable;
    std::unordered_map<std::string, Route> mPostTable;
};

#endif //WEBPROJECT_ROUTER_H
