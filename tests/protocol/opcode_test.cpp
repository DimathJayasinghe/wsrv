// SPDX-License-Identifier: Apache-2.0
#include "protocol/opcode.hpp"

#include <gtest/gtest.h>

namespace {

using wsrv::Opcode;

TEST(Opcode, OnlyTheSixDefinedOpcodesAreKnown) {  // EC-F02: 0x3-0x7 and 0xB-0xF are reserved
    for (unsigned value = 0; value < 16; ++value) {
        const auto opcode = wsrv::opcode_from_wire(static_cast<std::uint8_t>(value));
        const bool defined =
            value == 0 || value == 1 || value == 2 || value == 8 || value == 9 || value == 10;
        EXPECT_EQ(opcode.has_value(), defined) << value;
        if (defined) {
            EXPECT_EQ(static_cast<unsigned>(*opcode), value);
        }
    }
}

TEST(Opcode, ValuesAboveANibbleAreNeverKnown) {
    for (unsigned value = 16; value < 256; ++value) {
        EXPECT_FALSE(wsrv::opcode_from_wire(static_cast<std::uint8_t>(value)).has_value()) << value;
    }
}

TEST(Opcode, ControlFramesAreExactlyCloseAndPingAndPong) {
    EXPECT_TRUE(wsrv::is_control(Opcode::Close));
    EXPECT_TRUE(wsrv::is_control(Opcode::Ping));
    EXPECT_TRUE(wsrv::is_control(Opcode::Pong));
    EXPECT_FALSE(wsrv::is_control(Opcode::Continuation));
    EXPECT_FALSE(wsrv::is_control(Opcode::Text));
    EXPECT_FALSE(wsrv::is_control(Opcode::Binary));
}

}  // namespace
