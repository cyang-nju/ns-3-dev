get_filename_component(target_name ${CMAKE_CURRENT_SOURCE_DIR} NAME)

file(RELATIVE_PATH CURR_SOURCE_DIR_RELATIVE ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

set(json_hpp_filename nlohmann-json.hpp)
set(json_hpp_path ${CMAKE_CURRENT_SOURCE_DIR}/${json_hpp_filename})
if(NOT EXISTS ${json_hpp_path})
    set(json_hpp_path_rel ${CURR_SOURCE_DIR_RELATIVE}/${json_hpp_filename})
    message(STATUS "fetching ${json_hpp_path_rel} from https://github.com/nlohmann/json")
    file(DOWNLOAD
        https://github.com/nlohmann/json/raw/develop/single_include/nlohmann/json.hpp
        ${json_hpp_path}
        STATUS DOWNLOAD_STATUS
    )
    list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
    list(GET DOWNLOAD_STATUS 1 ERROR_MESSAGE)
    if(NOT ${STATUS_CODE} EQUAL 0)
        file(REMOVE ${json_hpp_path})
        message(FATAL_ERROR "Failed to fetch ${json_hpp_path_rel} from https://github.com/nlohmann/json: ${ERROR_MESSAGE}")
    endif()
endif()

file(GLOB_RECURSE src_files CONFIGURE_DEPENDS *.cc)

add_executable(${target_name} ${src_files})
target_compile_definitions(${target_name} PRIVATE  CURR_SRC_DIR_ABSOLUTE="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(${target_name} PRIVATE  CURR_SRC_DIR_RELATIVE="${CURR_SOURCE_DIR_RELATIVE}")
target_compile_definitions(${target_name} PRIVATE  ROOT_SRC_DIR="${CMAKE_SOURCE_DIR}")


if(${PRECOMPILE_HEADERS_ENABLED})
    target_precompile_headers(
        ${target_name} REUSE_FROM stdlib_pch_exec
    )
endif()

if(${NS3_STATIC})
    target_link_libraries(
        ${target_name} ${LIB_AS_NEEDED_PRE_STATIC}
        ${lib-ns3-static}
    )
elseif(${NS3_MONOLIB})
    target_link_libraries(
        ${target_name} ${LIB_AS_NEEDED_PRE}
        ${lib-ns3-monolib} ${LIB_AS_NEEDED_POST}
    )
else()
    target_link_libraries(
        ${target_name} ${LIB_AS_NEEDED_PRE}
        "${ns3-libs}" "${ns3-contrib-libs}"
        ${LIB_AS_NEEDED_POST}
    )
endif()

set_runtime_outputdirectory(
    ${target_name} ${CMAKE_OUTPUT_DIRECTORY}/ ""
)