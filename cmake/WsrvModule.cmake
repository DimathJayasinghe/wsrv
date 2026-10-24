# SPDX-License-Identifier: Apache-2.0
#
# wsrv_add_module(NAME <n> [SOURCES <files...>] [DEPS <targets...>])
#
# Creates library wsrv_<n> (alias wsrv::<n>). DEPS is the module's *allowed* dependency list: because
# CMake links exactly what is declared, an illegal dependency is a build error, not a review comment
# (docs/ARCHITECTURE.md section 2.1). A module with no SOURCES yet becomes an INTERFACE library so the
# graph can be declared before the code exists.
include_guard(GLOBAL)

function(wsrv_add_module)
    cmake_parse_arguments(ARG "" "NAME" "SOURCES;DEPS" ${ARGN})
    if(NOT ARG_NAME)
        message(FATAL_ERROR "wsrv_add_module: NAME is required")
    endif()
    set(tgt wsrv_${ARG_NAME})

    if(ARG_SOURCES)
        add_library(${tgt} STATIC ${ARG_SOURCES})
        target_include_directories(${tgt} PUBLIC "${PROJECT_SOURCE_DIR}/src")
        target_compile_features(${tgt} PUBLIC cxx_std_23)
        set_target_properties(${tgt} PROPERTIES CXX_EXTENSIONS OFF)
        wsrv_apply_warnings(${tgt})
        wsrv_apply_hardening(${tgt})
        if(ARG_DEPS)
            target_link_libraries(${tgt} PUBLIC ${ARG_DEPS})
        endif()
    else()
        add_library(${tgt} INTERFACE)
        target_include_directories(${tgt} INTERFACE "${PROJECT_SOURCE_DIR}/src")
        if(ARG_DEPS)
            target_link_libraries(${tgt} INTERFACE ${ARG_DEPS})
        endif()
    endif()
    add_library(wsrv::${ARG_NAME} ALIAS ${tgt})
endfunction()
