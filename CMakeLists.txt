cmake_minimum_required(VERSION 3.16)

project(DBLP_Extreme LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DBLP_EXTREME_SOURCES
    src/main.cpp
    src/ExtremeEngine.cpp
    src/ExtremeParser.cpp
    src/Analyzer.cpp
    src/F5Profile.cpp
    src/QuerySuite.cpp
)

add_executable(dblp_extreme ${DBLP_EXTREME_SOURCES})

target_include_directories(dblp_extreme PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(MSVC)
    target_compile_definitions(dblp_extreme PRIVATE NDEBUG)
    target_compile_options(dblp_extreme PRIVATE /O2 /EHsc /utf-8)
else()
    target_compile_definitions(dblp_extreme PRIVATE NDEBUG)
    # 下面两行已经安全移除了 -flto
    target_compile_options(dblp_extreme PRIVATE -O3 -march=native -pthread)
    target_link_options(dblp_extreme PRIVATE -pthread)
endif()