# SPDX-License-Identifier: Apache-2.0
# permessage-deflate (ADR-0020). The only module that includes <zlib.h>; zlib is a PRIVATE link so
# nothing that links wsrv::extensions inherits it. Without WSRV_ENABLE_DEFLATE the module is empty
# and the server never negotiates the extension.
if(WSRV_ENABLE_DEFLATE)
    wsrv_add_module(NAME extensions SOURCES zlib_codec.cpp DEPS wsrv::base wsrv::protocol)
    target_link_libraries(wsrv_extensions PRIVATE ZLIB::ZLIB)
else()
    wsrv_add_module(NAME extensions DEPS wsrv::base wsrv::protocol)
endif()
