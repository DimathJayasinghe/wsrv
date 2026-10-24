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
        # PUBLIC/INTERFACE links appear in INTERFACE_LINK_LIBRARIES; PRIVATE links of a STATIC library
        # appear in LINK_LIBRARIES (and as $<LINK_ONLY:...> in the interface list). Read both, and find
        # wsrv names *inside* generator expressions, so no kind of link can hide from the guard.
        set(_deps "")
        get_target_property(_type wsrv_${_m} TYPE)
        get_target_property(_iface wsrv_${_m} INTERFACE_LINK_LIBRARIES)
        if(_iface)
            list(APPEND _deps ${_iface})
        endif()
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_link wsrv_${_m} LINK_LIBRARIES)
            if(_link)
                list(APPEND _deps ${_link})
            endif()
        endif()
        set(_names "")
        foreach(_d IN LISTS _deps)
            string(REGEX MATCHALL "wsrv(::|_)[a-z0-9_]+" _hits "${_d}")
            foreach(_h IN LISTS _hits)
                string(REGEX REPLACE "^wsrv(::|_)" "" _n "${_h}")
                list(APPEND _names "${_n}")
            endforeach()
        endforeach()
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
