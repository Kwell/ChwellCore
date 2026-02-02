cmake_minimum_required(VERSION 3.10)

project(ChwellFrameCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CHWELL_BUILD_EXAMPLES "Build example servers" ON)

add_library(chwell_core
    src/core/logger.cpp
    src/core/config.cpp
    src/core/thread_pool.cpp
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
)

target_include_directories(chwell_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/generated/proto
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/asio/include
)

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
endif()

