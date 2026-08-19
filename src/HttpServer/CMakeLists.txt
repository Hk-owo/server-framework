add_library(HttpServerLib SHARED)

target_include_directories(HttpServerLib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_sources(HttpServerLib PRIVATE Httpserver.cpp Server.cpp Router.cpp)
target_link_libraries(HttpServerLib PUBLIC HttpLib ThreadPoolLib -fcoroutines)
target_precompile_headers(HttpServerLib PUBLIC ${PROJECT_SOURCE_DIR}/third_party/pch/pch.h)
