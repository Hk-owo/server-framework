//
// Created by lacas on 2026/3/10.
//

#ifndef WEBPROJECT_HTTPSERVER_H
#define WEBPROJECT_HTTPSERVER_H

#include "Server.h"
#include "HttpParser.h"
#include "HttpResponse.h"

/**
 * @brief 业务层 HTTP 服务器示例
 *
 * 基于自定义 Server 注册示例路由，展示框架用法。
 * 不包含任何数据库依赖，可直接编译运行。
 */
class HttpServer {
private:
    Server server;

    void registerRoutes();

public:
    HttpServer();
    ~HttpServer() = default;
};

#endif //WEBPROJECT_HTTPSERVER_H
