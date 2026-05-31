add_library(TimeWheelLib SHARED)

target_include_directories(TimeWheelLib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_sources(TimeWheelLib PRIVATE TimeWheel.cpp)
target_link_libraries(TimeWheelLib PUBLIC ThreadPoolLib uring)
target_precompile_headers(TimeWheelLib PUBLIC ${PROJECT_SOURCE_DIR}/third_party/pch/pch.h)