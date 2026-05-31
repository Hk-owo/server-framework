//
// Created by lacas on 2026/3/18.
//

#include "Server/HttpParser.h"

// ==================== HttpUtils 实现 ====================

namespace HttpUtils {
    void to_lower(std::string& s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    void trim(std::string& s) {
        auto not_space = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    }
}

// ==================== UriParse 实现 ====================

namespace UriParse {
    int hex_val(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    std::string url_decode(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '+') {
                out += ' ';
            } else if (s[i] == '%' && i + 2 < s.size()) {
                int hi = hex_val(s[i + 1]);
                int lo = hex_val(s[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out += static_cast<char>((hi << 4) | lo);
                    i += 2;
                } else {
                    out += s[i];
                }
            } else {
                out += s[i];
            }
        }
        return out;
    }

    ParsedUri parse_uri(const std::string& uri) {
        ParsedUri result;
        auto q = uri.find('?');
        if (q == std::string::npos) {
            result.path = uri;
            return result;
        }

        result.path = uri.substr(0, q);

        // 解析 key=value&key=value
        std::string_view qs(uri.data() + q + 1, uri.size() - q - 1);
        std::size_t i = 0;
        while (i < qs.size()) {
            auto amp = qs.find('&', i);
            auto seg = (amp == std::string::npos) ? qs.substr(i) : qs.substr(i, amp - i);

            auto eq = seg.find('=');
            if (eq != std::string::npos) {
                result.query.emplace(
                        url_decode(seg.substr(0, eq)),
                        url_decode(seg.substr(eq + 1))
                );
            }
            i = (amp == std::string::npos) ? qs.size() : amp + 1;
        }

        return result;
    }
}

// ==================== HttpRequest 实现 ====================

const UriParse::ParsedUri& HttpRequest::parsed_uri() const {
    if (!parsed_uri_) {
        parsed_uri_ = UriParse::parse_uri(uri);
    }
    return *parsed_uri_;
}

std::string HttpRequest::get_header_value(const std::string& name) const {
    std::string lower = name;
    HttpUtils::to_lower(lower);
    auto it = headers.find(lower);
    return (it != headers.end()) ? it->second : "";
}

std::string HttpRequest::get_param_value(const std::string& key) const {
    const auto& q = parsed_uri().query;
    auto it = q.find(key);
    return (it != q.end()) ? it->second : "";
}

// ==================== HttpParser 实现 ====================

void HttpParser::feed(const char* data, std::size_t len) {
    buf_.append(data, len);
}

void HttpParser::feed(std::string_view sv) {
    feed(sv.data(), sv.size());
}

std::optional<HttpRequest> HttpParser::try_parse() {
    HttpRequest req;

    if (!parse_request_line(req)) return std::nullopt;
    if (!parse_headers(req)) return std::nullopt;

    std::size_t consumed = 0;
    if (!parse_body(req, consumed)) return std::nullopt;

    buf_.erase(0, consumed);  // 只删掉这一个报文的数据，剩余的留着
    return req;
}

void HttpParser::reset() {
    buf_.clear();
}

bool HttpParser::parse_request_line(HttpRequest& req) {
    auto pos = buf_.find("\r\n");
    if (pos == std::string::npos) return false;

    std::string line = buf_.substr(0, pos);
    auto sp1 = line.find(' ');
    auto sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp1 == sp2) {
        throw std::runtime_error("Malformed request line");
    }

    req.method = line.substr(0, sp1);
    req.uri = line.substr(sp1 + 1, sp2 - sp1 - 1);
    req.version = line.substr(sp2 + 1);
    return true;
}

bool HttpParser::parse_headers(HttpRequest& req) {
    // 从 request line 之后开始找
    auto header_start = buf_.find("\r\n") + 2;
    auto header_end = buf_.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;  // header 还没收完

    std::string_view header_block(buf_.data() + header_start,
                                  header_end - header_start);

    // 逐行解析
    std::size_t i = 0;
    while (i < header_block.size()) {
        auto eol = header_block.find("\r\n", i);
        if (eol == std::string::npos) eol = header_block.size();

        auto colon = header_block.find(':', i);
        if (colon != std::string::npos && colon < eol) {
            std::string name(header_block.substr(i, colon - i));
            std::string value(header_block.substr(colon + 1, eol - colon - 1));
            HttpUtils::to_lower(name);
            HttpUtils::trim(value);
            req.headers[std::move(name)] = std::move(value);
        }
        i = (eol == std::string::npos) ? eol : eol + 2;
    }
    return true;
}

bool HttpParser::parse_body(HttpRequest& req, std::size_t& consumed) {
    auto header_end = buf_.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;

    std::size_t body_start = header_end + 4;

    std::string te = req.get_header_value("transfer-encoding");
    HttpUtils::to_lower(te);
    if (te.find("chunked") != std::string::npos) {
        return parse_chunked_body(req, body_start, consumed);
    }

    std::string cl_str = req.get_header_value("content-length");
    if (!cl_str.empty()) {
        std::size_t cl = 0;
        auto [ptr, ec] = std::from_chars(cl_str.data(),
                                         cl_str.data() + cl_str.size(), cl);
        if (ec != std::errc{}) throw std::runtime_error("Invalid Content-Length");
        if (buf_.size() < body_start + cl) return false;

        req.body = buf_.substr(body_start, cl);
        consumed = body_start + cl;
        return true;
    }

    // 无 body
    consumed = body_start;
    return true;
}

bool HttpParser::parse_chunked_body(HttpRequest& req, std::size_t pos, std::size_t& consumed) {
    while (true) {
        auto size_end = buf_.find("\r\n", pos);
        if (size_end == std::string::npos) return false;

        std::string size_str = buf_.substr(pos, size_end - pos);
        auto semi = size_str.find(';');
        if (semi != std::string::npos) size_str.resize(semi);

        std::size_t chunk_size = 0;
        for (char c : size_str) {
            chunk_size <<= 4;
            if (c >= '0' && c <= '9') chunk_size |= (c - '0');
            else if (c >= 'a' && c <= 'f') chunk_size |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') chunk_size |= (c - 'A' + 10);
            else throw std::runtime_error("Invalid chunk size");
        }

        if (chunk_size == 0) {
            // last-chunk 后还有 \r\n 结尾
            consumed = size_end + 2 + 2;  // "0\r\n" + trailer空行"\r\n"
            return true;
        }

        std::size_t data_start = size_end + 2;
        if (buf_.size() < data_start + chunk_size + 2) return false;

        req.body.append(buf_, data_start, chunk_size);
        pos = data_start + chunk_size + 2;
    }
}
