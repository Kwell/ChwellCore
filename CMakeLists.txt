cmake_minimum_required(VERSION 3.11)

project(ChwellFrameCore VERSION 0.1.0 LANGUAGES CXX)
include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# MSVC 默认按本地代码页解析源码，UTF-8 中文注释会被误解析；强制 UTF-8
if(MSVC)
    add_compile_options(/utf-8)
endif()

option(CHWELL_BUILD_EXAMPLES "Build example servers" ON)
option(CHWELL_USE_YAML "Enable YAML config for storage (yaml-cpp)" ON)
option(CHWELL_USE_PROTOBUF "Enable Protobuf codec and game.proto examples" ON)
option(CHWELL_USE_MYSQL "Enable MySQL storage backend" OFF)
option(CHWELL_USE_MONGODB "Enable MongoDB storage backend" OFF)
option(CHWELL_USE_OPENSSL "Enable OpenSSL for TLS support" OFF)
option(CHWELL_BUILD_TESTS "Build unit tests (GoogleTest)" ON)
option(CHWELL_USE_CONSUL "Build optional Consul discovery (libcurl + nlohmann-json)" OFF)
option(CHWELL_USE_ENTITY_SCHEMA "Build optional typed entity schema and visibility sync" OFF)

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
        target_include_directories(chwell_game_proto PUBLIC
            $<BUILD_INTERFACE:${CHWELL_PROTOBUF_GENERATED_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/chwell/proto>)
        target_link_libraries(chwell_game_proto PUBLIC protobuf::libprotobuf)
    else()
        message(WARNING "Protobuf not found; proto_frame examples will not use game.proto types")
        set(CHWELL_USE_PROTOBUF OFF)
    endif()
endif()


# yaml-cpp（优先 find_package，未找到则 FetchContent，需 CMake 3.14+）
set(CHWELL_BUNDLED_YAML OFF)
if(CHWELL_USE_YAML)
    find_package(yaml-cpp 0.6 QUIET)
    if(NOT yaml-cpp_FOUND AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.14)
        include(FetchContent)
        FetchContent_Declare(
            yaml-cpp
            GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
            GIT_TAG 0579ae3d976091d7d664aa9d2527e0d0cff25763
        )
        set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(yaml-cpp)
        set(CHWELL_BUNDLED_YAML ON)
    endif()
    if(NOT TARGET yaml-cpp::yaml-cpp)
        if(TARGET yaml-cpp)
            add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
        else()
            message(FATAL_ERROR "YAML requested but yaml-cpp target not found")
        endif()
    endif()
endif()

# MySQL
if(CHWELL_USE_MYSQL)
    find_package(ChwellMySQL REQUIRED)
endif()

# MongoDB
if(CHWELL_USE_MONGODB)
    find_package(ChwellMongoDB REQUIRED)
endif()

add_library(chwell_core
    src/core/logger.cpp
    src/core/config.cpp
    src/core/thread_pool.cpp
    src/core/timer_wheel.cpp
    src/net/posix_io.cpp
    src/net/tcp_server.cpp
    src/net/tcp_connection.cpp
    src/net/epoll_connection.cpp
    src/net/epoll_server.cpp
    src/net/udp_server.cpp
    src/net/udp_socket.cpp
    src/net/ws_connection.cpp
    src/net/ws_server.cpp
    src/net/tls.cpp
    src/net/connection_pool.cpp
    src/cluster/node.cpp
    src/cluster/discovery_router.cpp
    src/cluster/tcp_rpc_transport.cpp
    src/service/service.cpp
    src/service/app_host.cpp
    src/service/component_order.cpp
    src/service/plugin.cpp
    src/service/hot_reload.cpp
    src/protocol/message.cpp
    src/protocol/parser.cpp
    src/service/protocol_router.cpp
    src/benchmark/benchmark.cpp
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
    src/storage/async_storage_adapter.cpp
    src/aoi/aoi.cpp
    src/task/task_queue.cpp
    src/redis/redis_client.cpp
    src/cluster/mysql_session_store.cpp
    src/slg/map.cpp
    src/slg/battle.cpp
    src/game/game_components.cpp
    src/game/player_move.cpp
    src/sync/frame_sync.cpp
    src/sync/state_sync.cpp
    src/discovery/service_discovery.cpp
    src/loadbalance/load_balancer.cpp
    src/loadbalance/consistent_hash.cpp
    src/metrics/prometheus_metrics.cpp
    src/circuitbreaker/circuit_breaker.cpp
    src/ratelimit/rate_limiter.cpp
)

target_include_directories(chwell_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(chwell_core PUBLIC cxx_std_17)
target_link_libraries(chwell_core PUBLIC Threads::Threads PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(chwell_core PUBLIC $<$<BOOL:${CHWELL_USE_YAML}>:CHWELL_USE_YAML>)

target_compile_definitions(chwell_core PRIVATE
    $<$<BOOL:${CHWELL_USE_MYSQL}>:CHWELL_USE_MYSQL>
    $<$<BOOL:${CHWELL_USE_MONGODB}>:CHWELL_USE_MONGODB>
    $<$<BOOL:${CHWELL_USE_OPENSSL}>:CHWELL_USE_OPENSSL>
)

if(CHWELL_USE_YAML)
    target_link_libraries(chwell_core PUBLIC yaml-cpp::yaml-cpp)
endif()

if(CHWELL_USE_MYSQL)
    target_link_libraries(chwell_core PRIVATE ChwellDependency::MySQL)
endif()

if(CHWELL_USE_MONGODB)
    target_link_libraries(chwell_core PRIVATE ChwellDependency::MongoDB)
endif()

if(CHWELL_USE_OPENSSL)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(chwell_core PRIVATE OpenSSL::SSL)
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
            GIT_TAG f8d7d77c06936315286eb55f8de22cd23c188571
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
        if(NOT CHWELL_USE_YAML)
            message(STATUS "CHWELL_USE_YAML=OFF: Document 序列化使用 key=value 备用格式（ORM 单元测试仍覆盖往返）")
        endif()
        add_executable(chwell_core_tests
            tests/test_protocol_parser.cpp
            tests/test_protocol_router.cpp
            tests/test_orm_repository.cpp
            tests/test_persist_cache.cpp
            tests/test_cross_server.cpp
            tests/test_game_systems.cpp
            tests/test_social_match.cpp
            tests/test_anti_cheat.cpp
            tests/test_replay.cpp
            tests/test_load_bot.cpp
            tests/test_gray_release.cpp
            tests/test_ops_pay_codegen.cpp
            tests/test_gm_api_pay_channels.cpp
            tests/test_analytics_store.cpp
            tests/test_storage.cpp
            tests/test_session_manager.cpp
            tests/test_timer_wheel.cpp
            tests/test_aoi.cpp
            tests/test_event_bus.cpp
            tests/test_redis_client.cpp
            tests/test_object_pool.cpp
            tests/test_task_queue.cpp
            tests/test_logic_thread.cpp
            tests/test_observability.cpp
            tests/test_hot_reload.cpp
            tests/test_transaction.cpp
            tests/test_config.cpp
            tests/test_service_lifecycle.cpp
            tests/test_app_host.cpp
            tests/test_component_order.cpp
            tests/test_registrations.cpp
            tests/test_owned_registrations.cpp
            tests/test_slg.cpp
            tests/test_game_components.cpp
            tests/test_player_move.cpp
            tests/test_sync.cpp
            tests/test_discovery_loadbalance.cpp
            tests/test_discovery_router.cpp
            tests/test_tcp_rpc_transport.cpp
            tests/test_epoll_accept_order.cpp
            tests/test_circuitbreaker.cpp
            tests/test_ratelimit.cpp
            tests/test_consistent_hash.cpp
            tests/test_prometheus_metrics.cpp
            tests/test_benchmark.cpp
            tests/test_rpc.cpp
            tests/test_gateway_multinode.cpp
            tests/test_udp_socket.cpp
        )
        target_link_libraries(chwell_core_tests PRIVATE chwell_core ${CHWELL_GTEST_LIB})

        # 热加载测试插件（SHARED），供 test_hot_reload 加载
        add_library(test_hot_plugin SHARED tests/test_hot_plugin.cpp)
        target_include_directories(test_hot_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(test_hot_plugin PRIVATE ${CMAKE_DL_LIBS})
        add_dependencies(chwell_core_tests test_hot_plugin)
        target_compile_definitions(chwell_core_tests PRIVATE
            TEST_HOT_PLUGIN_PATH="$<TARGET_FILE:test_hot_plugin>")
        target_compile_definitions(chwell_core_tests PRIVATE
            $<$<BOOL:${CHWELL_USE_YAML}>:CHWELL_USE_YAML>
            $<$<BOOL:${CHWELL_USE_MYSQL}>:CHWELL_USE_MYSQL>
            $<$<BOOL:${CHWELL_USE_MONGODB}>:CHWELL_USE_MONGODB>
            $<$<BOOL:${CHWELL_USE_OPENSSL}>:CHWELL_USE_OPENSSL>
        )
        add_test(NAME chwell_core_tests COMMAND chwell_core_tests)

        # 集成测试
        add_executable(chwell_integration_tests
            integration_tests/test_integration.cpp
        )
        target_include_directories(chwell_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(chwell_integration_tests PRIVATE chwell_core ${CHWELL_GTEST_LIB})
        add_test(NAME chwell_integration_tests COMMAND chwell_integration_tests)
    else()
        message(WARNING "GoogleTest not found, CHWELL_BUILD_TESTS is ON but tests will not be built")
    endif()
endif()

if(CHWELL_USE_CONSUL)
    find_package(CURL 7.68 REQUIRED)
    find_package(nlohmann_json 3.11 REQUIRED)
    add_library(chwell_consul src/discovery/consul_discovery.cpp src/discovery/consul_http.cpp)
    target_link_libraries(chwell_consul PUBLIC chwell_core PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)
    if(CHWELL_BUILD_TESTS AND CHWELL_GTEST_LIB)
        add_executable(chwell_discovery_tests tests/test_consul_discovery.cpp)
        target_link_libraries(chwell_discovery_tests PRIVATE chwell_consul ${CHWELL_GTEST_LIB} nlohmann_json::nlohmann_json)
        add_test(NAME chwell_discovery_tests COMMAND chwell_discovery_tests)
    endif()
    if(CHWELL_BUILD_EXAMPLES)
        add_executable(example_discovery_cluster examples/discovery_cluster_demo.cpp)
        target_link_libraries(example_discovery_cluster PRIVATE chwell_consul nlohmann_json::nlohmann_json)
    endif()
endif()

if(CHWELL_USE_ENTITY_SCHEMA)
    add_library(chwell_schema src/schema/entity_schema.cpp src/sync/schema_sync.cpp)
    target_link_libraries(chwell_schema PUBLIC chwell_core)
    if(CHWELL_BUILD_TESTS OR CHWELL_BUILD_EXAMPLES)
        include(cmake/ChwellEntitySchema.cmake)
        chwell_generate_entity_schema(player_schema schemas/player.schema.json generated/schema/player_schema.h
            CSV schemas/player_defaults.csv)
    endif()
    if(CHWELL_BUILD_TESTS AND CHWELL_GTEST_LIB)
        chwell_generate_entity_schema(edge_schema tests/schema/edge.schema.json generated/schema/edge_schema.h)
        add_executable(chwell_schema_tests tests/test_entity_schema.cpp)
        add_dependencies(chwell_schema_tests player_schema edge_schema)
        target_include_directories(chwell_schema_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/schema)
        target_link_libraries(chwell_schema_tests PRIVATE chwell_schema ${CHWELL_GTEST_LIB})
        add_test(NAME chwell_schema_tests COMMAND chwell_schema_tests)
        find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
        add_test(NAME entity_schema_generator COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_entity_schema_tool.py)
    endif()
    if(CHWELL_BUILD_EXAMPLES)
        add_executable(example_entity_schema examples/entity_schema_demo.cpp)
        add_dependencies(example_entity_schema player_schema)
        target_include_directories(example_entity_schema PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/schema)
        target_link_libraries(example_entity_schema PRIVATE chwell_schema)
        if(CHWELL_BUILD_TESTS)
            add_test(NAME entity_schema_example COMMAND example_entity_schema)
        endif()
    endif()
endif()

# Stable consumer names work with both add_subdirectory and installed packages.
set(CHWELL_EXPORT_TARGETS chwell_core)
set_target_properties(chwell_core PROPERTIES EXPORT_NAME core)
add_library(Chwell::core ALIAS chwell_core)
foreach(module schema consul game_proto)
    if(TARGET chwell_${module})
        list(APPEND CHWELL_EXPORT_TARGETS chwell_${module})
        set_target_properties(chwell_${module} PROPERTIES EXPORT_NAME ${module})
        add_library(Chwell::${module} ALIAS chwell_${module})
    endif()
endforeach()
if(CHWELL_BUILD_EXAMPLES AND CHWELL_USE_CONSUL AND CHWELL_USE_ENTITY_SCHEMA AND CHWELL_USE_MYSQL)
    add_subdirectory(examples/cluster_reference)
endif()
if(CHWELL_BUNDLED_YAML)
    # The pinned upstream disables its install rules when included as a subproject.
    # Export the independently built dependency and retain its headers/license.
    list(APPEND CHWELL_EXPORT_TARGETS yaml-cpp)
    set_target_properties(yaml-cpp PROPERTIES EXPORT_NAME yaml_cpp)
    add_library(Chwell::yaml_cpp ALIAS yaml-cpp)
endif()
include(cmake/ChwellInstall.cmake)

# 复制 config 到 build 目录，便于 example_storage 从 build 运行
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/config DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

if (CHWELL_BUILD_EXAMPLES)
    add_executable(example_config_driven examples/config_driven_demo.cpp)
    target_link_libraries(example_config_driven PRIVATE chwell_core)

    add_executable(example_echo_server
        examples/echo_server.cpp
    )
    target_link_libraries(example_echo_server PRIVATE chwell_core)

    add_executable(example_protocol_server
        examples/protocol_server.cpp
    )
    target_link_libraries(example_protocol_server PRIVATE chwell_core)

    add_executable(bench_runner
        examples/bench_runner.cpp
    )
    target_link_libraries(bench_runner PRIVATE chwell_core)

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

    # Epoll 多 Reactor 压测工具
    add_executable(epoll_stress_test
        tests/epoll_stress_test.cpp
    )
    target_link_libraries(epoll_stress_test PRIVATE pthread)

    # 框架全链路压测（Service + ProtocolRouter 闭环 RTT）
    add_executable(framework_bench
        examples/framework_bench.cpp
    )
    target_link_libraries(framework_bench PRIVATE chwell_core)

    # Epoll Echo 服务器示例
    add_executable(epoll_echo_server
        examples/epoll_echo_server.cpp
    )
    target_link_libraries(epoll_echo_server PRIVATE chwell_core)

    # H5 小游戏 demo：游戏服 + 游戏网关
    add_executable(example_game_server
        examples/game_server.cpp
    )
    target_include_directories(example_game_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
    target_link_libraries(example_game_server PRIVATE chwell_core)

    add_executable(example_game_gateway_server
        examples/game_gateway_server.cpp
    )
    target_include_directories(example_game_gateway_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
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

    # GM 管理服务示例
    add_executable(example_gm_admin
        examples/gm_admin_demo.cpp
    )
    target_link_libraries(example_gm_admin PRIVATE chwell_core)

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

    # 协议压力测试客户端
    add_executable(example_protocol_stress_client
        examples/protocol_stress_client.cpp
    )
    target_link_libraries(example_protocol_stress_client PRIVATE pthread)

    # JSON 帧客户端（4字节大端长度前缀 + JSON 文本）
    add_executable(example_json_frame_client
        examples/json_frame_client.cpp
    )
    target_link_libraries(example_json_frame_client PRIVATE chwell_core)
endif()

