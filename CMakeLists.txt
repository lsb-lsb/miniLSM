cmake_minimum_required(VERSION 3.14)
project(miniLSM VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra")

# ==================== 源文件 ====================
set(MINILSM_SOURCES
    src/arena.cc
    src/skiplist.cc
    src/memtable.cc
    src/wal.cc
    src/bloom.cc
    src/sstable.cc
    src/sstable_builder.cc
    src/iterator.cc
    src/db.cc
    src/coding.cc
    src/status.cc
)

# ==================== 静态库 ====================
add_library(minilsm STATIC ${MINILSM_SOURCES})
target_include_directories(minilsm PUBLIC include)

# ==================== 测试（可选） ====================
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://gitee.com/mirrors/googletest.git
        GIT_TAG        v1.14.0
    )

    # 如果下载失败，跳过测试
    FetchContent_GetProperties(googletest)
    if(NOT googletest_POPULATED)
        FetchContent_Populate(googletest)
        add_subdirectory(${googletest_SOURCE_DIR} ${googletest_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()

    if(TARGET GTest::gtest AND TARGET GTest::gtest_main)
        enable_testing()

        function(add_minilsm_test NAME)
            add_executable(${NAME} test/${NAME}.cc)
            target_link_libraries(${NAME} minilsm GTest::gtest GTest::gtest_main)
            add_test(NAME ${NAME} COMMAND ${NAME})
        endfunction()

        add_minilsm_test(skiplist_test)
        add_minilsm_test(memtable_test)
        add_minilsm_test(wal_test)
        add_minilsm_test(sstable_test)
        add_minilsm_test(bloom_test)
        add_minilsm_test(db_test)
        add_minilsm_test(concurrency_test)

        message(STATUS "Google Test enabled — unit tests available")
    else()
        message(WARNING "Google Test not available — unit tests disabled")
    endif()
endif()

# ==================== Benchmark ====================
add_executable(benchmark test/benchmark.cc)
target_link_libraries(benchmark minilsm)


