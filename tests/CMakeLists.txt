# 测试程序：协议层独立测试，仅依赖 HttpLib（不含服务器/日志）
add_executable(test_http_parser scripts/test_http_parser.cpp)
target_include_directories(test_http_parser PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_http_parser PUBLIC HttpLib)
