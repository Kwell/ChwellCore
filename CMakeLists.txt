cmake_minimum_required(VERSION 3.11)

project(ChwellFrameCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CHWELL_BUILD_EXAMPLES "Build example servers" ON)
option(CHWELL_USE_YAML "Enable YAML config for storage (yaml-cpp)" ON)
option(CHWELL_USE_PROTOBUF "Enable Protobuf codec and game.proto examples" ON)
option(CHWELL_USE_MYSQL "Enable MySQL storage backend" OFF)
option(CHWELL_USE_MONGODB "Enable MongoDB storage backend" OFF)
option(CHWELL_USE_OPENSSL "Enable OpenSSL for TLS support" OFF)
option(CHWELL_BUILD_TESTS "Build unit tests (GoogleTest)" ON)

if(CHWELL_USE_PROTOBUF)
    find_package(Protobuf QUIET)
    if(Protobuf_FOUND)
        set(CHWELL_PROTOBUF_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/proto")
        file(MAKE_DIRECTORY "${CHWELL_PROTOBUF_GENERATED_DIR}")
        set(CHWELL_PROTO_GAME_PB_CC "${CHWELL_PROTOBUF_GENERATED_DIR}/game.pb.cc")
        set(CHWELL_PROTO_GAME_PB_H "${CHWELL_PROTOBUF_GENERATED_DIR}/game.pb.h")
        add_custom_command(OUTPUT "${CHWELL_PROTO_GAME_PB_CC}" "${CHWELL_PROTO_GAME_PB_H}"
            COMMAND ${Protobuf_PROTOC_EXECUTABLE}
                -I "${CMAKE_CURRENT_SOURCE_DIR}/proto"
                --cpp_out "${CHWELL_PROTOBUF_GENERATED_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/proto/game.proto"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/proto/game.proto"
            COMMENT "Generating game.pb.cc/h")
        add_library(chwell_game_proto STATIC "${CHWELL_PROTO_GAME_PB_CC}")
        target_include_directories(chwell_game_proto PUBLIC "${CHWELL_PROTOBUF_GENERATED_DIR}" ${Protobuf_INCLUDE_DIRS})
        target_link_libraries(chwell_game_proto PUBLIC ${Protobuf_LIBRARIES})
    else()
        message(WARNING "Protobuf not found; proto_frame examples will not use game.proto types")
        set(CHWELL_USE_PROTOBUF OFF)
    endif()
endif()


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
    src/core/timer_wheel.cpp
    src/net/posix_io.cpp
    src/net/tcp_server.cpp
    src/net/tcp_connection.cpp
    src/net/udp_server.cpp
    src/net/ws_connection.cpp
    src/net/ws_server.cpp
    src/net/tls.cpp
    src/net/connection_pool.cpp
    src/cluster/node.cpp
    src/service/service.cpp
    src/protocol/message.cpp
    src/protocol/parser.cpp
    src/service/protocol_router.cpp
    src/http/http_response.cpp
    src/http/http_server.cpp
    src/codec/codec.cpp
    src/rpc/rpc_client.cpp
    src/rpc/rpc_server.cpp
    src/gateway/gateway_forwarder.cpp
    src/storage/memory_storage.cpp
    src/storage/mysql_storage.cpp
    src/storage/mongodb_storage.cpp
    src/storage/storage_factory.cpp
    src/storage/storage_component.cpp
    src/storage/storage_config_loader.cpp
    src/storage/orm/document.cpp
    src/aoi/aoi.cpp
    src/task/task_queue.cpp
    src/redis/redis_client.cpp
    src/slg/map.cpp
    src/slg/battle.cpp
    src/game/game_components.cpp
    src/game/player_move.cpp
    src/sync/frame_sync.cpp
    src/sync/state_sync.cpp
    src/discovery/service_discovery.cpp
    src/loadbalance/load_balancer.cpp
    src/circuitbreaker/circuit_breaker.cpp
    src/ratelimit/rate_limiter.cpp
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
    $<$<BOOL:${CHWELL_USE_OPENSSL}>:CHWELL_USE_OPENSSL>
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

if(CHWELL_USE_OPENSSL)
    find_package(OpenSSL QUIET)
    if(OPENSSL_FOUND)
        target_link_libraries(chwell_core PRIVATE OpenSSL::SSL)
    else()
        message(WARNING "CHWELL_USE_OPENSSL=ON but OpenSSL not found, TLS wrapper will be stub")
    endif()
endif()

if (CHWELL_BUILD_TESTS)
    enable_testing()

    # 优先使用系统安装的 GTest，其次使用 FetchContent 拉取
    find_package(GTest QUIET)

    if (NOT GTest_FOUND AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.14)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    set(CHWELL_GTEST_LIB)
    if (TARGET GTest::gtest AND TARGET GTest::gtest_main)
        set(CHWELL_GTEST_LIB GTest::gtest GTest::gtest_main)
    elseif (TARGET gtest AND TARGET gtest_main)
        set(CHWELL_GTEST_LIB gtest gtest_main)
    elseif (GTest_FOUND)
        set(CHWELL_GTEST_LIB GTest::GTest GTest::Main)
    endif()

    if (CHWELL_GTEST_LIB)
        add_executable(chwell_core_tests
            tests/test_protocol_parser.cpp
            tests/test_protocol_router.cpp
            tests/test_orm_repository.cpp
            tests/test_session_manager.cpp
            tests/test_timer_wheel.cpp
            tests/test_aoi.cpp
            tests/test_event_bus.cpp
            tests/test_redis_client.cpp
            tests/test_object_pool.cpp
            tests/test_task_queue.cpp
            tests/test_slg.cpp
            tests/test_game_components.cpp
            tests/test_player_move.cpp
            tests/test_sync.cpp
            tests/test_discovery_loadbalance.cpp
            tests/test_circuitbreaker.cpp
            tests/test_ratelimit.cpp
        )
        target_link_libraries(chwell_core_tests PRIVATE chwell_core ${CHWELL_GTEST_LIB})
        add_test(NAME chwell_core_tests COMMAND chwell_core_tests)

        # 集成测试
        add_executable(chwell_integration_tests
            integration_tests/test_integration.cpp
        )
        target_link_libraries(chwell_integration_tests PRIVATE chwell_core ${CHWELL_GTEST_LIB})
        add_test(NAME chwell_integration_tests COMMAND chwell_integration_tests)
    else()
        message(WARNING "GoogleTest not found, CHWELL_BUILD_TESTS is ON but tests will not be built")
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

    add_executable(example_orm
        examples/orm_example.cpp
    )
    target_link_libraries(example_orm PRIVATE chwell_core)

    # 纯 Protobuf 帧服务示例（方式 B）：不使用 protocol::Message/cmd 头，
    # 顶层协议直接为 ProtobufCodec 定义的 varint32 长度前缀帧。
    add_executable(example_proto_frame_server
        examples/proto_frame_server.cpp
    )
    target_link_libraries(example_proto_frame_server PRIVATE chwell_core)

    add_executable(example_proto_frame_client
        examples/proto_frame_client.cpp
    )
    target_link_libraries(example_proto_frame_client PRIVATE chwell_core)

    # H5 小游戏 demo：游戏服 + 游戏网关
    add_executable(example_game_server
        examples/game_server.cpp
    )
    target_include_directories(example_game_server PRIVATE ${CMAKE_SOURCE_DIR}/examples)
    target_link_libraries(example_game_server PRIVATE chwell_core)

    add_executable(example_game_gateway_server
        examples/game_gateway_server.cpp
    )
    target_include_directories(example_game_gateway_server PRIVATE ${CMAKE_SOURCE_DIR}/examples)
    target_link_libraries(example_game_gateway_server PRIVATE chwell_core)

    # WebSocket-TCP 桥接（H5 浏览器连 WS，转发到网关 TCP）
    find_package(OpenSSL QUIET)
    if(OPENSSL_FOUND)
        add_executable(ws_tcp_bridge
            examples/ws_tcp_bridge.cpp
        )
        target_include_directories(ws_tcp_bridge PRIVATE ${OPENSSL_INCLUDE_DIR})
        target_link_libraries(ws_tcp_bridge PRIVATE OpenSSL::SSL)
    else()
        message(WARNING "OpenSSL not found, ws_tcp_bridge will not be built (required for H5 game demo)")
    endif()

    # WebSocket + Protobuf 游戏服务器
    add_executable(example_game_ws_server
        examples/game_ws_server.cpp
    )

    # 查找 OpenSSL（用于 WebSocket 握手的 SHA1）
    find_package(OpenSSL)
    if(OPENSSL_FOUND)
        target_link_libraries(example_game_ws_server PRIVATE chwell_core OpenSSL::Crypto)
    else()
        target_link_libraries(example_game_ws_server PRIVATE chwell_core)
    endif()

    # 新增模块示例
    add_executable(example_new_modules
        examples/new_modules_demo.cpp
    )
    target_link_libraries(example_new_modules PRIVATE chwell_core)
    target_compile_features(example_new_modules PRIVATE cxx_std_11)

    # 游戏组件示例
    add_executable(example_game_components_demo
        examples/game_components_demo.cpp
    )
    target_link_libraries(example_game_components_demo PRIVATE chwell_core)

    # 帧同步 + 状态同步示例
    add_executable(example_sync_demo
        examples/sync_demo.cpp
    )
    target_link_libraries(example_sync_demo PRIVATE chwell_core)

    # WebSocket + 游戏组件示例
    add_executable(example_game_ws_components_demo
        examples/game_ws_components_demo.cpp
    )
    target_link_libraries(example_game_ws_components_demo PRIVATE chwell_core)
endif()

