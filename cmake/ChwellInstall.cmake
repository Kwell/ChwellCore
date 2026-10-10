include(CMakePackageConfigHelpers)
set(CHWELL_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/ChwellCore")
set(CHWELL_TOOL_DIR "${CMAKE_INSTALL_DATADIR}/ChwellCore/tools")
configure_package_config_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ChwellCoreConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ChwellCoreConfig.cmake"
    INSTALL_DESTINATION "${CHWELL_PACKAGE_DIR}" PATH_VARS CHWELL_TOOL_DIR)
# Pre-1.0 minor versions are separate compatibility lines. This is not an ABI promise.
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/ChwellCoreConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion)
install(TARGETS ${CHWELL_EXPORT_TARGETS} EXPORT ChwellCoreTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/chwell DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
if(CHWELL_BUNDLED_YAML)
    install(DIRECTORY "${yaml-cpp_SOURCE_DIR}/include/yaml-cpp" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(FILES "${yaml-cpp_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/ChwellCore/licenses" RENAME yaml-cpp-LICENSE)
endif()
if(TARGET chwell_game_proto)
    install(FILES "${CHWELL_PROTO_GAME_PB_H}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/chwell/proto")
endif()
install(EXPORT ChwellCoreTargets NAMESPACE Chwell:: DESTINATION "${CHWELL_PACKAGE_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ChwellCoreConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ChwellCoreConfigVersion.cmake"
    cmake/ChwellEntitySchema.cmake cmake/FindChwellMySQL.cmake cmake/FindChwellMongoDB.cmake
    DESTINATION "${CHWELL_PACKAGE_DIR}")
install(FILES tools/entity_schema.py DESTINATION "${CHWELL_TOOL_DIR}")
install(DIRECTORY examples/reference_service/ DESTINATION "${CMAKE_INSTALL_DATADIR}/ChwellCore/examples/reference_service")
install(DIRECTORY examples/cluster_reference/ DESTINATION "${CMAKE_INSTALL_DATADIR}/ChwellCore/examples/cluster_reference")
install(FILES docs/PACKAGING.md docs/ENTITY_SCHEMA.md docs/APP_HOST.md docs/REGISTRATIONS.md docs/SESSION_OWNERSHIP.md docs/SYNC_PROTOCOL.md
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ChwellCore/docs")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/ChwellCore")
