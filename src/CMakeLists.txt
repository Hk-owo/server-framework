add_subdirectory(Logger)
add_subdirectory(ThreadPool)
add_subdirectory(TimeWheel)
add_subdirectory(HttpServer)

add_executable(${PROJECT_NAME} main.cpp)
target_include_directories(${PROJECT_NAME} PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(${PROJECT_NAME} PUBLIC HttpServerLib)
target_precompile_headers(${PROJECT_NAME} PUBLIC ${PROJECT_SOURCE_DIR}/third_party/pch/pch.h)
