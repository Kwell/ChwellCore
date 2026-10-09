cmake_minimum_required(VERSION 3.14)
project(ChwellConfigTests LANGUAGES CXX)

# 独立验证配置模块，无需构建 POSIX 网络层或可选存储依赖。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()
if(TARGET GTest::gtest_main)
    set(CHWELL_CONFIG_GTEST GTest::gtest_main)
elseif(TARGET gtest_main)
    set(CHWELL_CONFIG_GTEST gtest_main)
else()
    set(CHWELL_CONFIG_GTEST GTest::Main)
endif()

find_package(Threads REQUIRED)
add_executable(chwell_config_tests
    ../test_config.cpp
    ../../src/core/config.cpp
    ../../src/core/logger.cpp)
target_include_directories(chwell_config_tests PRIVATE ../../include)
target_link_libraries(chwell_config_tests PRIVATE ${CHWELL_CONFIG_GTEST} Threads::Threads)
if(MSVC)
    target_compile_definitions(chwell_config_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(chwell_config_tests PRIVATE /W4)
else()
    target_compile_options(chwell_config_tests PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME chwell_config_tests COMMAND chwell_config_tests)
