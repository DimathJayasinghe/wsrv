// SPDX-License-Identifier: Apache-2.0
#include "net/io_error.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <vector>

namespace {

using wsrv::ErrorCategory;
using wsrv::ErrorCode;

struct Mapping {
    int err;
    ErrorCode expected;
};

TEST(IoError, MapsTheErrnoValuesTheServerCaresAbout) {
    const std::vector<Mapping> table{{EAGAIN, ErrorCode::WouldBlock},
                                     {EWOULDBLOCK, ErrorCode::WouldBlock},
                                     {EPIPE, ErrorCode::ConnectionClosed},
                                     {ECONNRESET, ErrorCode::ConnectionClosed},
                                     {ECONNABORTED, ErrorCode::ConnectionClosed},
                                     {ETIMEDOUT, ErrorCode::ConnectionClosed},
                                     {ENOTCONN, ErrorCode::ConnectionClosed},
                                     {EMFILE, ErrorCode::FdLimit},
                                     {ENFILE, ErrorCode::FdLimit},
                                     {ENOBUFS, ErrorCode::FdLimit},
                                     {ENOMEM, ErrorCode::FdLimit},
                                     {EADDRINUSE, ErrorCode::AddressInUse},
                                     {EIO, ErrorCode::Io},
                                     {EBADF, ErrorCode::Io},
                                     {EACCES, ErrorCode::Io},
                                     {9999, ErrorCode::Io}};
    for (const Mapping& m : table) {
        EXPECT_EQ(wsrv::map_errno(m.err, "ctx").code, m.expected) << m.err;
    }
}

TEST(IoError, KeepsTheContextAndNeverAllocates) {
    const wsrv::Error e = wsrv::map_errno(EPIPE, "write to client");
    EXPECT_EQ(e.context, "write to client");
}

TEST(IoError, IoErrorsHaveTheIoCategoryAndResourceExhaustionIsALimit) {
    EXPECT_EQ(wsrv::category(ErrorCode::WouldBlock), ErrorCategory::Io);
    EXPECT_EQ(wsrv::category(ErrorCode::ConnectionClosed), ErrorCategory::Io);
    EXPECT_EQ(wsrv::category(ErrorCode::AddressInUse), ErrorCategory::Io);
    EXPECT_EQ(wsrv::category(ErrorCode::Io), ErrorCategory::Io);
    EXPECT_EQ(wsrv::category(ErrorCode::FdLimit), ErrorCategory::Limit);
}

}  // namespace
