# Reusable build-time generator. Python is required only for generation.
if(NOT CHWELL_ENTITY_SCHEMA_TOOL)
    set(CHWELL_ENTITY_SCHEMA_TOOL "${CMAKE_CURRENT_LIST_DIR}/../tools/entity_schema.py")
endif()
function(chwell_generate_entity_schema target schema output)
    cmake_parse_arguments(SCHEMA "" "CSV;XLSX;SHEET;CSHARP_OUTPUT;PREVIOUS;CATALOG" "" ${ARGN})
    if(SCHEMA_UNPARSED_ARGUMENTS OR SCHEMA_KEYWORDS_MISSING_VALUES OR
        (SCHEMA_CSV AND SCHEMA_CATALOG) OR (SCHEMA_XLSX AND SCHEMA_CATALOG) OR
        (SCHEMA_CSV AND SCHEMA_XLSX) OR (SCHEMA_SHEET AND NOT SCHEMA_XLSX))
        message(FATAL_ERROR "Schema generation: unknown arguments, conflicting content inputs or SHEET without XLSX")
    endif()
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    get_filename_component(schema "${schema}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(output "${output}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    set(arguments "${schema}" --output "${output}")
    set(dependencies "${schema}" "${CHWELL_ENTITY_SCHEMA_TOOL}")
    set(outputs "${output}")
    if(SCHEMA_CSHARP_OUTPUT)
        get_filename_component(csharp "${SCHEMA_CSHARP_OUTPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
        list(APPEND outputs "${csharp}")
        list(APPEND arguments --csharp-output "${csharp}")
    endif()
    if(SCHEMA_SHEET)
        list(APPEND arguments --sheet "${SCHEMA_SHEET}")
    endif()
    foreach(option CSV XLSX PREVIOUS CATALOG)
        if(SCHEMA_${option})
            get_filename_component(input "${SCHEMA_${option}}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            string(TOLOWER "${option}" flag)
            list(APPEND arguments "--${flag}" "${input}")
            list(APPEND dependencies "${input}")
            if(option STREQUAL "CATALOG")
                execute_process(COMMAND "${Python3_EXECUTABLE}" "${CHWELL_ENTITY_SCHEMA_TOOL}"
                    --catalog "${input}" --list-inputs
                    RESULT_VARIABLE result OUTPUT_VARIABLE inputs ERROR_VARIABLE diagnostic
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
                if(NOT result EQUAL 0)
                    message(FATAL_ERROR "Invalid content catalog: ${diagnostic}")
                endif()
                string(REPLACE "\r\n" "\n" inputs "${inputs}")
                string(REPLACE "\n" ";" inputs "${inputs}")
                list(APPEND dependencies ${inputs})
                # Adding/removing tables must update the build graph too.
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                    "${input}" "${CHWELL_ENTITY_SCHEMA_TOOL}")
            endif()
        endif()
    endforeach()
    add_custom_command(OUTPUT ${outputs}
        COMMAND "${Python3_EXECUTABLE}" "${CHWELL_ENTITY_SCHEMA_TOOL}" ${arguments}
        DEPENDS ${dependencies} VERBATIM COMMENT "Generating validated entity schema")
    add_custom_target(${target} DEPENDS ${outputs})
endfunction()

# One package contains the entire validated table graph. Content version is
# independent of each compiled schema version.
function(chwell_generate_content_bundle target catalog output)
    cmake_parse_arguments(CONTENT "" "VERSION" "" ${ARGN})
    if(CONTENT_UNPARSED_ARGUMENTS OR CONTENT_KEYWORDS_MISSING_VALUES OR NOT CONTENT_VERSION)
        message(FATAL_ERROR "Content bundle generation requires VERSION")
    endif()
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    get_filename_component(catalog "${catalog}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(output "${output}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CHWELL_ENTITY_SCHEMA_TOOL}"
        --catalog "${catalog}" --list-inputs
        RESULT_VARIABLE result OUTPUT_VARIABLE inputs ERROR_VARIABLE diagnostic
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Invalid content catalog: ${diagnostic}")
    endif()
    string(REPLACE "\r\n" "\n" inputs "${inputs}")
    string(REPLACE "\n" ";" inputs "${inputs}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${catalog}" "${CHWELL_ENTITY_SCHEMA_TOOL}")
    # Make/Ninja must rebuild even when only VERSION changes during configure.
    file(GENERATE OUTPUT "${output}.version" CONTENT "${CONTENT_VERSION}\n")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CHWELL_ENTITY_SCHEMA_TOOL}"
            --catalog "${catalog}" --bundle-output "${output}" --content-version "${CONTENT_VERSION}"
        DEPENDS "${catalog}" "${CHWELL_ENTITY_SCHEMA_TOOL}" "${output}.version" ${inputs}
        VERBATIM COMMENT "Generating versioned runtime content package")
    add_custom_target(${target} DEPENDS "${output}")
endfunction()
