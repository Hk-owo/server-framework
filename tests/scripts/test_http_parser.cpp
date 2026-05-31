//
// HTTP Parser Unit Test
//

#include <iostream>
#include <cassert>
#include "Server/HttpParser.h"

using namespace std;

void test_request_line() {
    HttpParser parser;
    parser.feed("GET /hello?name=world HTTP/1.1\r\nHost: localhost\r\n\r\n");
    auto req = parser.try_parse();
    assert(req.has_value());
    assert(req->method == "GET");
    assert(req->uri == "/hello?name=world");
    assert(req->parsed_uri().path == "/hello");
    assert(req->get_param_value("name") == "world");
    assert(req->get_header_value("Host") == "localhost");
    cout << "✅ test_request_line passed" << endl;
}

void test_post_body() {
    HttpParser parser;
    parser.feed(
        "POST /echo HTTP/1.1\r\n"
        "Content-Length: 4\r\n"
        "\r\n"
        "ping"
    );
    auto req = parser.try_parse();
    assert(req.has_value());
    assert(req->method == "POST");
    assert(req->body == "ping");
    cout << "✅ test_post_body passed" << endl;
}

void test_partial_feed() {
    HttpParser parser;
    parser.feed("GET /json HT");
    assert(!parser.try_parse().has_value());
    parser.feed("TP/1.1\r\nHost: localhost\r\n\r\n");
    auto req = parser.try_parse();
    assert(req.has_value());
    assert(req->method == "GET");
    cout << "✅ test_partial_feed passed" << endl;
}

int main() {
    cout << "========================================" << endl;
    cout << " HTTP Parser Unit Tests" << endl;
    cout << "========================================" << endl;

    try {
        test_request_line();
        test_post_body();
        test_partial_feed();
        cout << "\n========================================" << endl;
        cout << " All tests passed!" << endl;
        cout << "========================================" << endl;
        return 0;
    } catch (const exception& e) {
        cerr << "❌ Test failed: " << e.what() << endl;
        return 1;
    }
}
