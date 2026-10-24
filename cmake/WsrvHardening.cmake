# SPDX-License-Identifier: Apache-2.0
#
# Exploit-mitigation flags for optimised builds (docs/CODING_STANDARDS.md section 1).
include_guard(GLOBAL)

function(wsrv_apply_hardening target)
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        return()
    endif()

    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)

    set(_cfg "$<CONFIG:Release,RelWithDebInfo,MinSizeRel>")
    # -U first: some distros already define _FORTIFY_SOURCE; redefining it would warn (and we use -Werror).
    target_compile_options(${target} PRIVATE
        "$<${_cfg}:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=3;-fstack-protector-strong;-fstack-clash-protection>")

    # Control-flow protection is architecture specific.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
        target_compile_options(${target} PRIVATE "$<${_cfg}:-fcf-protection>")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        target_compile_options(${target} PRIVATE "$<${_cfg}:-mbranch-protection=standard>")
    endif()

    # Link-time hardening is applied to executables via wsrv_harden_executable().
endfunction()

function(wsrv_harden_executable target)
    set(_cfg "$<CONFIG:Release,RelWithDebInfo,MinSizeRel>")
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_options(${target} PRIVATE "$<${_cfg}:-pie;-Wl,-z,relro,-z,now>")
endfunction()
