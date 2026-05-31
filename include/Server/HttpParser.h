//
// Created by lacas on 2026/3/18.
//

#ifndef WEBPROJECT_HTTPPARSER_H
#define WEBPROJECT_HTTPPARSER_H


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
    // 追加原始数据，可以是任意分片
    void feed(const char* data, std::size_t len);
    void feed(std::string_view sv);

    std::optional<HttpRequest> try_parse();

    void reset();

private:
    bool parse_request_line(HttpRequest& req);
    bool parse_headers(HttpRequest& req);
    bool parse_body(HttpRequest& req, std::size_t& consumed);
    bool parse_chunked_body(HttpRequest& req, std::size_t pos, std::size_t& consumed);

    std::string buf_;
};

#endif //WEBPROJECT_HTTPPARSER_H
