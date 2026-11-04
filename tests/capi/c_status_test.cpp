// SPDX-License-Identifier: Apache-2.0
#include "capi/c_status.hpp"

#include <gtest/gtest.h>

namespace {

using namespace wsrv;

TEST(CStatus, TheCasesAHostActsOnKeepTheirOwnCode) {
    EXPECT_EQ(to_c_status(ErrorCode::Closing), WSRV_ERR_NO_SUCH_CONN);
    EXPECT_EQ(to_c_status(ErrorCode::Backpressure), WSRV_ERR_BACKPRESSURE);
    EXPECT_EQ(to_c_status(ErrorCode::BufferFull), WSRV_ERR_BACKPRESSURE);
    EXPECT_EQ(to_c_status(ErrorCode::InvalidUtf8), WSRV_ERR_INVALID_UTF8);
    EXPECT_EQ(to_c_status(ErrorCode::AddressInUse), WSRV_ERR_ADDRESS_IN_USE);
    EXPECT_EQ(to_c_status(ErrorCode::InvalidConfig), WSRV_ERR_INVALID_CONFIG);
    EXPECT_EQ(to_c_status(ErrorCode::WrongProcess), WSRV_ERR_WRONG_PROCESS);
    EXPECT_EQ(to_c_status(ErrorCode::InvalidArgument), WSRV_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(to_c_status(ErrorCode::ResourceExhausted), WSRV_ERR_RESOURCE);
}

TEST(CStatus, EveryErrorCodeMapsToAnErrorAndNeverToOkOrTimeout) {
    for (unsigned raw = 0; raw <= static_cast<unsigned>(ErrorCode::InvalidCompressedData); ++raw) {
        const wsrv_status status = to_c_status(static_cast<ErrorCode>(raw));
        EXPECT_GE(status, WSRV_ERR_INVALID_ARGUMENT) << "code " << raw;
        EXPECT_LE(status, WSRV_ERR_INTERNAL) << "code " << raw;
    }
}

}  // namespace
