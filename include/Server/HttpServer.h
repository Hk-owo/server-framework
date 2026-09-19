//
// Created by lacas on 2026/3/10.
//

#ifndef WEBPROJECT_HTTPSERVER_H
#define WEBPROJECT_HTTPSERVER_H

#include "Server.h"

#include <memory>
#include <thread>
#include <vector>

/**
 * @brief 业务层 HTTP 服务器示例
 *
 * 基于自定义 Server 注册示例路由，展示框架用法。
 * 不包含任何数据库依赖，可直接编译运行。
 *
 * 启动 N 个 Server 实例，每个持有私有的 io_uring ring、连接表与时间轮，
 * 它们通过 SO_REUSEPORT 绑定同一端口，由内核按连接 4 元组哈希分发请求——
 * 同一连接恒定落到同一实例，因此实例之间不需要任何状态同步。
 * 实例数由环境变量 SF_LOOPS 指定，默认取 CPU 核数。
 */
class HttpServer {
private:
    std::vector<std::shared_ptr<Server>> mServers;
    std::vector<std::thread>             mLoops;

    // 路由表是每实例一份的只读副本（各自注册，运行期不改）
    void registerRoutes(Server& server);

public:
    HttpServer();
    ~HttpServer() = default;
};

#endif //WEBPROJECT_HTTPSERVER_H
