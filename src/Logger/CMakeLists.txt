add_library(LoggerLib SHARED)

find_package(spdlog 1.12 REQUIRED)

target_sources(LoggerLib PRIVATE Logger.cpp)

target_include_directories(LoggerLib PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(LoggerLib PUBLIC spdlog::spdlog)

target_precompile_headers(LoggerLib PUBLIC ${PROJECT_SOURCE_DIR}/third_party/pch/pch.h)