cmake_minimum_required(VERSION 3.11)

project(ChwellFrameCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CHWELL_BUILD_EXAMPLES "Build example servers" ON)
option(CHWELL_USE_YAML "Enable YAML config for storage (yaml-cpp)" ON)
option(CHWELL_USE_MYSQL "Enable MySQL storage backend" OFF)
option(CHWELL_USE_MONGODB "Enable MongoDB storage backend" OFF)

# yaml-cpp（优先 find_package，未找到则 FetchContent，需 CMake 3.14+）
if(CHWELL_USE_YAML)
    find_package(yaml-cpp 0.6 QUIET)
    if(NOT yaml-cpp_FOUND AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.14)
        include(FetchContent)
        FetchContent_Declare(
            yaml-cpp
            GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
            GIT_TAG yaml-cpp-0.7.0
        )
        set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(yaml-cpp)
    endif()
    if(NOT TARGET yaml-cpp AND NOT yaml-cpp_FOUND)
        message(WARNING "yaml-cpp not found, install libyaml-cpp-dev or use CMake 3.14+ for FetchContent")
    endif()
endif()

# MySQL
if(CHWELL_USE_MYSQL)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MYSQL QUIET mysqlclient)
    endif()
    if(NOT MYSQL_FOUND)
        find_path(MYSQL_INCLUDE_DIR mysql/mysql.h)
        find_library(MYSQL_LIBRARY NAMES mysqlclient mysqlclient_r)
        if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
            set(MYSQL_FOUND TRUE)
        endif()
    endif()
endif()

# MongoDB
if(CHWELL_USE_MONGODB)
    find_package(mongoc-1.0 QUIET)
    if(NOT mongoc-1.0_FOUND)
        find_package(libmongoc-1.0 QUIET)
    endif()
    if(NOT mongoc-1.0_FOUND AND NOT libmongoc-1.0_FOUND)
        find_path(MONGOC_INCLUDE_DIR mongoc/mongoc.h PATH_SUFFIXES libmongoc-1.0)
        find_library(MONGOC_LIBRARY NAMES mongoc-1.0)
        find_library(BSON_LIBRARY NAMES bson-1.0)
        if(MONGOC_INCLUDE_DIR AND MONGOC_LIBRARY AND BSON_LIBRARY)
            set(mongoc-1.0_FOUND TRUE)
        endif()
    endif()
endif()

add_library(chwell_core
    src/core/logger.cpp
    src/core/config.cpp
    src/core/thread_pool.cpp
    src/net/posix_io.cpp
    src/net/tcp_server.cpp
    src/net/tcp_connection.cpp
    src/net/udp_server.cpp
    src/net/ws_connection.cpp
    src/net/ws_server.cpp
    src/cluster/node.cpp
    src/service/service.cpp
    src/protocol/message.cpp
    src/protocol/parser.cpp
    src/service/protocol_router.cpp
    src/http/http_response.cpp
    src/http/http_server.cpp
    src/codec/codec.cpp
    src/rpc/rpc_client.cpp
    src/gateway/gateway_forwarder.cpp
    src/storage/memory_storage.cpp
    src/storage/mysql_storage.cpp
    src/storage/mongodb_storage.cpp
    src/storage/storage_factory.cpp
    src/storage/storage_component.cpp
    src/storage/storage_config_loader.cpp
)

target_include_directories(chwell_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/generated/proto
)

target_compile_definitions(chwell_core PRIVATE
    $<$<BOOL:${CHWELL_USE_YAML}>:CHWELL_USE_YAML>
    $<$<BOOL:${CHWELL_USE_MYSQL}>:CHWELL_USE_MYSQL>
    $<$<BOOL:${CHWELL_USE_MONGODB}>:CHWELL_USE_MONGODB>
)

if(CHWELL_USE_YAML)
    if(TARGET yaml-cpp::yaml-cpp)
        target_link_libraries(chwell_core PRIVATE yaml-cpp::yaml-cpp)
    elseif(TARGET yaml-cpp)
        target_link_libraries(chwell_core PRIVATE yaml-cpp)
    elseif(yaml-cpp_FOUND)
        target_link_libraries(chwell_core PRIVATE ${YAML_CPP_LIBRARIES})
        target_include_directories(chwell_core PRIVATE ${YAML_CPP_INCLUDE_DIR})
    endif()
endif()

if(CHWELL_USE_MYSQL AND MYSQL_FOUND)
    target_include_directories(chwell_core PRIVATE ${MYSQL_INCLUDE_DIRS})
    target_link_libraries(chwell_core PRIVATE ${MYSQL_LIBRARIES})
endif()

if(CHWELL_USE_MONGODB)
    if(TARGET mongoc_shared OR TARGET mongoc_static)
        target_link_libraries(chwell_core PRIVATE mongoc_shared)
    elseif(mongoc-1.0_FOUND OR libmongoc-1.0_FOUND)
        target_include_directories(chwell_core PRIVATE ${MONGOC_INCLUDE_DIRS})
        target_link_libraries(chwell_core PRIVATE ${MONGOC_LIBRARIES} ${BSON_LIBRARIES})
    elseif(MONGOC_INCLUDE_DIR AND MONGOC_LIBRARY AND BSON_LIBRARY)
        target_include_directories(chwell_core PRIVATE ${MONGOC_INCLUDE_DIR})
        target_link_libraries(chwell_core PRIVATE ${MONGOC_LIBRARY} ${BSON_LIBRARY})
    else()
        message(WARNING "CHWELL_USE_MONGODB=ON but mongoc not found, MongoDB storage will be stub")
    endif()
endif()

# 复制 config 到 build 目录，便于 example_storage 从 build 运行
file(COPY ${CMAKE_SOURCE_DIR}/config DESTINATION ${CMAKE_BINARY_DIR})

if (CHWELL_BUILD_EXAMPLES)
    add_executable(example_echo_server
        examples/echo_server.cpp
    )
    target_link_libraries(example_echo_server PRIVATE chwell_core)

    add_executable(example_protocol_server
        examples/protocol_server.cpp
    )
    target_link_libraries(example_protocol_server PRIVATE chwell_core)

    add_executable(example_http_server
        examples/http_server.cpp
    )
    target_link_libraries(example_http_server PRIVATE chwell_core)

    add_executable(example_gateway_server
        examples/gateway_server.cpp
    )
    target_link_libraries(example_gateway_server PRIVATE chwell_core)

    add_executable(example_storage
        examples/storage_example.cpp
    )
    target_link_libraries(example_storage PRIVATE chwell_core)
endif()

