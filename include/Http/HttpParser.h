//
// Created by lacas on 2026/3/18.
//

#ifndef WEBPROJECT_HTTPPARSER_H
#define WEBPROJECT_HTTPPARSER_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>


namespace HttpUtils {
    void to_lower(std::string& s);
    void trim(std::string& s);
}

// ==================== URI 解析 ====================

namespace UriParse {
    struct ParsedUri {
        std::string path;
        std::unordered_map<std::string, std::string> query;
    };

    int hex_val(char c);
    std::string url_decode(std::string_view s);
    ParsedUri parse_uri(const std::string& uri);
}

// ==================== HTTP 请求结构 ====================

struct HttpRequest {
    std::string method;
    std::string uri;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    // 懒解析，第一次调用时才拆 uri
    const UriParse::ParsedUri& parsed_uri() const;

    std::string get_header_value(const std::string& name) const;
    std::string get_param_value(const std::string& key) const;

private:
    mutable std::optional<UriParse::ParsedUri> parsed_uri_;
};

// ==================== HTTP 解析器 ====================

class HttpParser {
public:
    // 累积缓冲上限：一条连接上"已经收到、但还没能切出完整请求"的字节数上限。
    // 没有它，一个只发半截请求（或声明超大 Content-Length）的客户端就能把 buf_
    // 撑到任意大——解析器会一直 append 等它把请求发完。超过上限后 feed 不再收任何
    // 字节，只置 overflowed()，由上层回 413 并关闭连接
    static constexpr std::size_t kMaxBufferSize = 1 << 20;   // 1MB，与 nginx client_max_body_size 同量级

    // 追加原始数据，可以是任意分片
    void feed(const char* data, std::size_t len);
    void feed(std::string_view sv);

    std::optional<HttpRequest> try_parse();

    // 累积是否已超限（超限后 buf_ 冻结在超限前的内容，不再增长）
    bool overflowed() const noexcept { return overflowed_; }

    void reset();

private:
    bool parse_request_line(HttpRequest& req);
    bool parse_headers(HttpRequest& req);
    bool parse_body(HttpRequest& req, std::size_t& consumed);
    bool parse_chunked_body(HttpRequest& req, std::size_t pos, std::size_t& consumed);

    std::string buf_;
    bool        overflowed_ = false;
};

#endif //WEBPROJECT_HTTPPARSER_H
