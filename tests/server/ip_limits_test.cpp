// SPDX-License-Identifier: Apache-2.0
// Per-IP connection cap and new-connection rate (ADR-0021): what one address may hold and how fast
// it may knock, how addresses are compared (IPv4, IPv4-mapped IPv6, IPv6 /64), and that the table
// stays bounded and fails closed. A fake clock drives the token bucket; no sockets.
#include "server/ip_limits.hpp"

#include "net/endpoint.hpp"
#include "support/fakes.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wsrv::Admission;
using wsrv::Endpoint;
using wsrv::IpLimits;
using wsrv::IpLimitsConfig;
using wsrv::test::FakeClock;

Endpoint at(std::string_view ip, std::uint16_t port = 1000) {
    auto parsed = wsrv::parse_endpoint(ip, port);
    EXPECT_TRUE(parsed.has_value()) << ip;
    return *parsed;
}

IpLimitsConfig config(std::size_t per_ip, std::uint32_t per_second, std::size_t tracked = 1000) {
    IpLimitsConfig c;
    c.max_connections_per_ip = per_ip;
    c.max_new_connections_per_second = per_second;
    c.max_tracked = tracked;
    c.hash_seed = 0x1234;
    return c;
}

TEST(IpLimits, AnAddressMayHoldTheCapAndNoMoreUntilOneEnds) {  // REQUIREMENTS section 4: per IP
    FakeClock clock;
    IpLimits limits{config(3, 0), clock};
    const Endpoint client = at("203.0.113.5");
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(limits.admit(client), Admission::Admitted) << i;
    }
    EXPECT_EQ(limits.admit(client), Admission::TooManyConnections);
    limits.release(client);
    EXPECT_EQ(limits.admit(client), Admission::Admitted);
    EXPECT_EQ(limits.admit(client), Admission::TooManyConnections);
}

TEST(IpLimits, TheCapIsPerAddressNotGlobal) {
    FakeClock clock;
    IpLimits limits{config(1, 0), clock};
    EXPECT_EQ(limits.admit(at("203.0.113.5")), Admission::Admitted);
    EXPECT_EQ(limits.admit(at("203.0.113.6")), Admission::Admitted);
    EXPECT_EQ(limits.admit(at("203.0.113.5", 2000)), Admission::TooManyConnections)
        << "the port is not part of who the client is";
}

TEST(IpLimits, ZeroSwitchesALimitOff) {
    FakeClock clock;
    IpLimits limits{config(0, 0), clock};
    const Endpoint client = at("203.0.113.5");
    for (int i = 0; i < 5000; ++i) {
        ASSERT_EQ(limits.admit(client), Admission::Admitted) << i;
    }
}

TEST(IpLimits, TheBucketAllowsABurstOfOneSecondsWorthThenRefillsAtTheRate) {
    FakeClock clock;
    IpLimits limits{config(0, 5), clock};  // 5 new connections per second, no cap
    const Endpoint client = at("203.0.113.5");
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(limits.admit(client), Admission::Admitted) << i;
    }
    EXPECT_EQ(limits.admit(client), Admission::RateExceeded);

    clock.advance(199ms);  // not yet one token (5 per second = one per 200 ms)
    EXPECT_EQ(limits.admit(client), Admission::RateExceeded);
    clock.advance(1ms);
    EXPECT_EQ(limits.admit(client), Admission::Admitted);
    EXPECT_EQ(limits.admit(client), Admission::RateExceeded);
}

TEST(IpLimits, ManyShortStepsStillRefillNoTimeIsLostToRounding) {
    FakeClock clock;
    IpLimits limits{config(0, 10), clock};  // one token per 100 ms
    const Endpoint client = at("203.0.113.5");
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(limits.admit(client), Admission::Admitted);
    }
    for (int i = 0; i < 100; ++i) {  // 100 half-millisecond steps = 50 ms: no token yet
        clock.advance(500us);
        ASSERT_EQ(limits.admit(client), Admission::RateExceeded) << i;
    }
    for (int i = 0; i < 100; ++i) {  // another 50 ms: exactly one token has accumulated
        clock.advance(500us);
        if (limits.admit(client) == Admission::Admitted) {
            SUCCEED();
            return;
        }
    }
    FAIL() << "100 ms of polling every 0.5 ms never earned the token";
}

TEST(IpLimits, AfterALongSilenceTheBucketIsFullNotOverflowing) {
    FakeClock clock;
    IpLimits limits{config(0, 1'000'000), clock};  // the highest configurable rate
    const Endpoint client = at("203.0.113.5");
    EXPECT_EQ(limits.admit(client), Admission::Admitted);
    clock.advance(std::chrono::hours{24 * 365});  // a year: elapsed * rate would be huge
    for (int i = 0; i < 1'000'000; ++i) {         // exactly the capacity, then refused
        ASSERT_EQ(limits.admit(client), Admission::Admitted) << i;
    }
    EXPECT_EQ(limits.admit(client), Admission::RateExceeded);
}

TEST(IpLimits, AClockThatGoesBackwardsGrantsNothingAndBreaksNothing) {
    FakeClock clock;
    clock.advance(10s);
    IpLimits limits{config(0, 2), clock};
    const Endpoint client = at("203.0.113.5");
    EXPECT_EQ(limits.admit(client), Admission::Admitted);
    EXPECT_EQ(limits.admit(client), Admission::Admitted);
    clock.advance(-5s);
    EXPECT_EQ(limits.admit(client), Admission::RateExceeded);
    clock.advance(5s);
    EXPECT_EQ(limits.admit(client), Admission::RateExceeded)
        << "no time passed since the last grant";
}

TEST(IpLimits, ARefusalDoesNotCostASlotOrAToken) {
    FakeClock clock;
    IpLimits limits{config(2, 100), clock};
    const Endpoint client = at("203.0.113.5");
    EXPECT_EQ(limits.admit(client), Admission::Admitted);
    EXPECT_EQ(limits.admit(client), Admission::Admitted);
    for (int i = 0; i < 500; ++i) {  // hammering a full address must not drain its bucket
        ASSERT_EQ(limits.admit(client), Admission::TooManyConnections);
    }
    limits.release(client);
    EXPECT_EQ(limits.admit(client), Admission::Admitted) << "the bucket was not spent on refusals";
}

TEST(IpLimits, TheSameClientIsRecognisedWhateverItsAddressFamilySpelling) {  // EC-N18
    FakeClock clock;
    IpLimits limits{config(1, 0), clock};
    EXPECT_EQ(limits.admit(at("203.0.113.5")), Admission::Admitted);
    EXPECT_EQ(limits.admit(at("::ffff:203.0.113.5")), Admission::TooManyConnections)
        << "an IPv4-mapped address is the IPv4 client, not a second budget";
    EXPECT_EQ(limits.admit(at("::ffff:203.0.113.6")), Admission::Admitted);
}

TEST(IpLimits, AnIpv6ClientIsItsWholeSlash64) {  // EC-N18
    FakeClock clock;
    IpLimits limits{config(1, 0), clock};
    EXPECT_EQ(limits.admit(at("2001:db8:1:2::1")), Admission::Admitted);
    EXPECT_EQ(limits.admit(at("2001:db8:1:2:ffff:ffff:ffff:ffff")), Admission::TooManyConnections)
        << "rotating through the interface identifiers must not buy more connections";
    EXPECT_EQ(limits.admit(at("2001:db8:1:3::1")), Admission::Admitted) << "a different /64";
    EXPECT_EQ(limits.admit(at("::1")), Admission::Admitted) << "loopback is its own client";
}

TEST(IpLimits, AnIpv4AddressAndTheSameBytesInIpv6AreDifferentClients) {
    FakeClock clock;
    IpLimits limits{config(1, 0), clock};
    EXPECT_EQ(limits.admit(at("1.2.3.4")), Admission::Admitted);
    EXPECT_EQ(limits.admit(at("102:304::")), Admission::Admitted);  // bytes 01 02 03 04 00 ...
}

TEST(IpLimits, TheTableIsBoundedAndARefusalIsTheOverflowAction) {  // every table has a bound
    FakeClock clock;
    IpLimits limits{config(5, 0, /*tracked=*/4), clock};
    for (int i = 1; i <= 4; ++i) {
        ASSERT_EQ(limits.admit(at("198.51.100." + std::to_string(i))), Admission::Admitted);
    }
    EXPECT_EQ(limits.tracked(), 4U);
    EXPECT_EQ(limits.admit(at("198.51.100.99")), Admission::TableFull);
    EXPECT_EQ(limits.tracked(), 4U) << "a refused newcomer is not remembered";
    limits.release(at("198.51.100.1"));  // its last connection ends: the entry goes at once
    EXPECT_EQ(limits.tracked(), 3U);
    EXPECT_EQ(limits.admit(at("198.51.100.99")), Admission::Admitted);
}

TEST(IpLimits, EntriesThatOnlyRememberADrainedBucketAreSweptWhenTheTableIsFull) {
    FakeClock clock;
    IpLimits limits{config(0, 2, /*tracked=*/3), clock};
    for (int i = 1; i <= 3; ++i) {  // three clients each take a token and leave
        const Endpoint client = at("198.51.100." + std::to_string(i));
        ASSERT_EQ(limits.admit(client), Admission::Admitted);
        limits.release(client);
    }
    EXPECT_EQ(limits.tracked(), 3U) << "their buckets are not full yet: the entries stay";
    EXPECT_EQ(limits.admit(at("198.51.100.50")), Admission::TableFull)
        << "still within the second: nothing to sweep";
    clock.advance(1s);  // every bucket is full again: those entries carry no information
    EXPECT_EQ(limits.admit(at("198.51.100.50")), Admission::Admitted);
    EXPECT_EQ(limits.tracked(), 1U);
}

TEST(IpLimits, AnIdleServerHoldsNoPerAddressState) {
    FakeClock clock;
    IpLimits limits{config(10, 0), clock};
    for (int i = 1; i <= 50; ++i) {
        const Endpoint client = at("192.0.2." + std::to_string(i));
        ASSERT_EQ(limits.admit(client), Admission::Admitted);
        limits.release(client);
    }
    EXPECT_EQ(limits.tracked(), 0U);
}

TEST(IpLimits, ReleasingWhatWasNeverAdmittedIsHarmless) {
    FakeClock clock;
    IpLimits limits{config(1, 0), clock};
    limits.release(at("203.0.113.5"));  // unknown address
    EXPECT_EQ(limits.admit(at("203.0.113.5")), Admission::Admitted);
    limits.release(at("203.0.113.5"));
    limits.release(at("203.0.113.5"));  // one release too many
    EXPECT_EQ(limits.admit(at("203.0.113.5")), Admission::Admitted);
    EXPECT_EQ(limits.admit(at("203.0.113.5")), Admission::TooManyConnections)
        << "the extra release must not have made room for a second connection";
}

TEST(IpLimits, AdmitAndReleaseFromManyThreadsKeepTheCountExact) {  // run under TSan
    FakeClock clock;
    IpLimits limits{config(8, 0), clock};
    const Endpoint client = at("203.0.113.5");
    std::atomic<int> over{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 2000; ++i) {
                if (limits.admit(client) == Admission::Admitted) {
                    limits.release(client);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(over.load(), 0);
    EXPECT_EQ(limits.tracked(), 0U);
    for (int i = 0; i < 8; ++i) {  // nothing leaked: the whole cap is available again
        ASSERT_EQ(limits.admit(client), Admission::Admitted) << i;
    }
    EXPECT_EQ(limits.admit(client), Admission::TooManyConnections);
}

}  // namespace
