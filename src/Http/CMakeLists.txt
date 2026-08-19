add_library(HttpLib SHARED)

target_include_directories(HttpLib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_sources(HttpLib PRIVATE HttpParser.cpp HttpResponse.cpp)
