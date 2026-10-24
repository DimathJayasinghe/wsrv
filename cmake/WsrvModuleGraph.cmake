# SPDX-License-Identifier: Apache-2.0
#
# wsrv_write_module_graph(<output-file> MODULES <names...>)
#
# Writes one line per module: "<module>: <sorted wsrv dependencies>". A module whose target does not
# exist is written as "<module>: <MISSING>". tests/module_graph.expected holds the graph that
# docs/ARCHITECTURE.md section 2.1 allows; the CTest 'module_graph' diffs the two, so any added,
# removed or forbidden dependency fails a test instead of relying on a reviewer's memory.
include_guard(GLOBAL)

function(wsrv_write_module_graph output)
    cmake_parse_arguments(ARG "" "" "MODULES" ${ARGN})
    set(_text "")
    foreach(_m IN LISTS ARG_MODULES)
        if(NOT TARGET wsrv_${_m})
            string(APPEND _text "${_m}: <MISSING>\n")
            continue()
        endif()
        get_target_property(_deps wsrv_${_m} INTERFACE_LINK_LIBRARIES)
        set(_names "")
        if(_deps)
            foreach(_d IN LISTS _deps)
                if(_d MATCHES "^wsrv(::|_)(.+)$")
                    list(APPEND _names "${CMAKE_MATCH_2}")
                endif()
            endforeach()
        endif()
        list(SORT _names)
        list(REMOVE_DUPLICATES _names)
        list(JOIN _names " " _joined)
        if(_joined)
            string(APPEND _text "${_m}: ${_joined}\n")
        else()
            string(APPEND _text "${_m}:\n")
        endif()
    endforeach()
    file(WRITE "${output}" "${_text}")
endfunction()
