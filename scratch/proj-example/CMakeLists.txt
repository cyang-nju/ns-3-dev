get_filename_component(target_name ${CMAKE_CURRENT_SOURCE_DIR} NAME)

file(GLOB_RECURSE src_files CONFIGURE_DEPENDS *.cc)

add_executable(${target_name} ${src_files})


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