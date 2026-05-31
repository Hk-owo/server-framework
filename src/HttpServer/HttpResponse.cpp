//
// Created by lacas on 2026/3/18.
//

#include "Server/HttpResponse.h"

// ── 设置接口实现 ───────────────────────────────────────────────────────────

HttpResponse& HttpResponse::set_status(int code) {
    code_ = code;
    reason_ = default_reason(code);
    return *this;
}

HttpResponse& HttpResponse::set_status(int code, std::string reason) {
    code_ = code;
    reason_ = std::move(reason);
    return *this;
}

HttpResponse& HttpResponse::set_header(std::string name, std::string value) {
    to_lower(name);
    headers_[std::move(name)] = std::move(value);
    return *this;
}

HttpResponse& HttpResponse::set_body(std::string body, std::string content_type) {
    body_ = std::move(body);
    headers_["content-type"]   = std::move(content_type);
    headers_["content-length"] = std::to_string(body_.size());
    return *this;
}

HttpResponse& HttpResponse::set_json(std::string json_body) {
    return set_body(std::move(json_body), "application/json");
}

// ── 获取接口实现 ───────────────────────────────────────────────────────────

std::string HttpResponse::build() const {
    if (code_ == 0) {
        throw std::runtime_error("status code not set");
    }

    std::ostringstream ss;
    ss << "HTTP/1.1 " << code_ << " " << reason_ << "\r\n";

    for (const auto& [k, v] : headers_) {
        ss << k << ": " << v << "\r\n";
    }

    ss << "\r\n";
    ss << body_;
    return ss.str();
}

// ── 辅助方法实现 ───────────────────────────────────────────────────────────

void HttpResponse::to_lower(std::string& s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
}

std::string HttpResponse::default_reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}
