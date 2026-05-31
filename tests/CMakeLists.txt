# 测试程序
add_executable(test_http_parser scripts/test_http_parser.cpp)
target_include_directories(test_http_parser PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_precompile_headers(test_http_parser PUBLIC ${PROJECT_SOURCE_DIR}/third_party/pch/pch.h)
target_link_libraries(test_http_parser PUBLIC HttpServerLib)
