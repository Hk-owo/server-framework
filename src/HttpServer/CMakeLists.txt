add_library(HttpServerLib SHARED)

target_include_directories(HttpServerLib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_sources(HttpServerLib PRIVATE Httpserver.cpp Server.cpp HttpParser.cpp HttpResponse.cpp)
target_link_libraries(HttpServerLib PUBLIC TimeWheelLib -fcoroutines)
target_precompile_headers(HttpServerLib PUBLIC ${PROJECT_SOURCE_DIR}/third_party/pch/pch.h)
