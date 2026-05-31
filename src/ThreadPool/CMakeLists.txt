add_library(ThreadPoolLib SHARED)

target_include_directories(ThreadPoolLib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_sources(ThreadPoolLib PRIVATE ThreadPool.cpp)
target_link_libraries(ThreadPoolLib PUBLIC LoggerLib uring)
target_precompile_headers(ThreadPoolLib PUBLIC ${PROJECT_SOURCE_DIR}/third_party/pch/pch.h)