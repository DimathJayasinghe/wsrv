// SPDX-License-Identifier: Apache-2.0
#include "base/logger.hpp"

#include "support/fakes.hpp"

#include <gtest/gtest.h>

TEST(Logger, NullLoggerAcceptsEveryLevel) {
    wsrv::NullLogger logger;
    for (auto level : {wsrv::LogLevel::Trace,
                       wsrv::LogLevel::Debug,
                       wsrv::LogLevel::Info,
                       wsrv::LogLevel::Warn,
                       wsrv::LogLevel::Error}) {
        logger.log(level, "event", "message");
    }
    SUCCEED();
}

TEST(Logger, RecordingLoggerKeepsLevelAndMessageInOrder) {
    wsrv::test::RecordingLogger logger;
    wsrv::ILogger& as_interface = logger;
    as_interface.log(wsrv::LogLevel::Warn, "first_event", "first");
    as_interface.log(wsrv::LogLevel::Error, "second_event", "second");
    ASSERT_EQ(logger.entries.size(), 2U);
    EXPECT_EQ(logger.entries[0].level, wsrv::LogLevel::Warn);
    EXPECT_EQ(logger.entries[0].message, "first");
    EXPECT_EQ(logger.entries[1].message, "second");
}
