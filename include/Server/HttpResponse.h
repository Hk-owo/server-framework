//
// Created by lacas on 2026/3/18.
//

#ifndef WEBPROJECT_HTTPRESPONSE_H
#define WEBPROJECT_HTTPRESPONSE_H

class HttpResponse {
public:
    // ── 设置接口 ────────────────────────────────────────────────────────────

    HttpResponse& set_status(int code);
    HttpResponse& set_status(int code, std::string reason);
    HttpResponse& set_header(std::string name, std::string value);
    HttpResponse& set_body(std::string body, std::string content_type = "text/plain");
    HttpResponse& set_json(std::string json_body);

    // ── 获取接口 ────────────────────────────────────────────────────────────

    // 构建完整报文，交给 send() 发送
    std::string build() const;

private:
    int         code_   = 0;
    std::string reason_;
    std::string body_;
    std::unordered_map<std::string, std::string> headers_;

    static void to_lower(std::string& s);
    static std::string default_reason(int code);
};

#endif //WEBPROJECT_HTTPRESPONSE_H
