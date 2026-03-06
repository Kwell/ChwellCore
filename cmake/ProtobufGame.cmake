# Protobuf game.proto generation
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
