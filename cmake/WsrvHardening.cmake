# SPDX-License-Identifier: Apache-2.0
#
# Exploit-mitigation flags for optimised builds (docs/CODING_STANDARDS.md section 1).
#
# Applied GLOBALLY (to our code, tests AND third-party dependencies, before any target exists):
#  * a per-target approach left executables and dependencies unprotected, and distro compiler defaults
#    (Ubuntu enables most of these) hid that;
#  * control-flow protection (CET/IBT) survives linking only if EVERY object has it, so one unflagged
#    dependency object would silently remove it from the whole binary.
# tests/tools/check_release_flags.py and check_binary_hardening.sh verify the result in Release builds.
include_guard(GLOBAL)

include(CheckPIESupported)
check_pie_supported(LANGUAGES CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)   # -fPIE for executables (+ -pie at link), -fPIC for libraries

set(_wsrv_opt "$<CONFIG:Release,RelWithDebInfo,MinSizeRel>")

# -U first: some distros already define _FORTIFY_SOURCE; redefining it would warn (and we use -Werror).
# _FORTIFY_SOURCE needs optimisation, hence optimised configurations only.
add_compile_options(
    "$<${_wsrv_opt}:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=3;-fstack-protector-strong;-fstack-clash-protection>")

# The standard library's own bounds checks: an out-of-range std::span / vector / string_view access
# aborts instead of being silent undefined behaviour (the debug presets already have them). libstdc++
# reads _GLIBCXX_ASSERTIONS, libc++ reads its hardening mode; each ignores the other's macro. Global for
# the same reason as above: the macro changes class layouts' checks, so every object must agree.
add_compile_options(
    "$<${_wsrv_opt}:-D_GLIBCXX_ASSERTIONS;-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>")

# Control-flow protection is architecture specific.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
    add_compile_options("$<${_wsrv_opt}:-fcf-protection>")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    add_compile_options("$<${_wsrv_opt}:-mbranch-protection=standard>")
endif()

# Full RELRO: GOT is read-only after startup (relro + immediate binding).
add_link_options("$<${_wsrv_opt}:-Wl,-z,relro,-z,now>")
