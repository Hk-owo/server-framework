//
// Created by lacas on 2026/8/19.
//

#include "Server/Router.h"

#include <utility>

void Router::add(std::unordered_map<std::string, Route>& table,
                 std::string pattern, Handler handler, bool async) {
    // 确保路径以/开头
    if (pattern.empty() || pattern[0] != '/') {
        pattern = "/" + pattern;
    }
    table[std::move(pattern)] = Route{std::move(handler), async};
}

void Router::Get(std::string pattern, Handler handler) {
    add(mGetTable, std::move(pattern), std::move(handler), false);
}

void Router::Post(std::string pattern, Handler handler) {
    add(mPostTable, std::move(pattern), std::move(handler), false);
}

void Router::GetAsync(std::string pattern, Handler handler) {
    add(mGetTable, std::move(pattern), std::move(handler), true);
}

void Router::PostAsync(std::string pattern, Handler handler) {
    add(mPostTable, std::move(pattern), std::move(handler), true);
}

bool Router::match(const HttpRequest& req, Route& out) const {
    const auto& path = req.parsed_uri().path;
    const auto& table = (req.method == "GET") ? mGetTable : mPostTable;

    // 精确匹配
    if (auto it = table.find(path); it != table.end()) {
        out = it->second;
        return true;
    }

    // 前缀匹配：pattern 以 '/' 结尾时按目录前缀匹配
    for (const auto& [pattern, route] : table) {
        if (pattern.back() == '/' && path.rfind(pattern, 0) == 0) {
            out = route;
            return true;
        }
    }
    return false;
}
