// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/clock.hpp"
#include "net/endpoint.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace wsrv {

/// What one client address may do (ADR-0021). `0` switches a limit off.
struct IpLimitsConfig {
    std::size_t max_connections_per_ip = 100;           ///< live connections from one address key
    std::uint32_t max_new_connections_per_second = 50;  ///< token bucket: capacity = the rate
    std::size_t max_tracked = 20000;                    ///< bound of the table (keys held at once)
    std::uint64_t hash_seed = 0;  ///< per-server secret: see the class comment
};

/// The verdict of IpLimits::admit(). Only `Admitted` holds a slot, which release() must give back.
enum class Admission : std::uint8_t {
    Admitted,
    TooManyConnections,  ///< the address already holds max_connections_per_ip
    RateExceeded,        ///< the address opened connections faster than the bucket refills
    TableFull,           ///< a new address while the table is at its bound (or out of memory)
};

/// Per-address connection count and new-connection rate, shared by the accept path (loop 0) and
/// every loop that ends a connection (ADR-0021 options 3). One mutex, held for a single map
/// operation and never across a call out; connection and protocol code never touches it.
///
/// What "the same client" means (EC-N18): IPv4 by its 4 bytes; an IPv4-mapped IPv6 address as the
/// IPv4 address it maps; any other IPv6 address by its /64 prefix. The port is ignored.
///
/// The table is bounded by `max_tracked`: a new address when it is full is refused after one
/// sweep of entries that carry no information (no connection, bucket full again). The hash is
/// keyed with `hash_seed` so that an attacker who picks source addresses cannot make the table
/// degenerate; the seed must come from the OS random source in production.
///
/// Thread-affinity: any thread.
class IpLimits {
public:
    IpLimits(const IpLimitsConfig& config, const IClock& clock);

    /// Decide whether `peer` may have one more connection and, if so, count it.
    [[nodiscard]] Admission admit(const Endpoint& peer);

    /// Give back the slot of one admitted connection of `peer`. Extra or unknown releases do
    /// nothing, so a bookkeeping slip can never grant more than the configured cap.
    void release(const Endpoint& peer) noexcept;

    /// Addresses currently remembered. For tests and metrics.
    [[nodiscard]] std::size_t tracked() const;

    /// The comparison key of an address (exposed for the tests of the normalisation).
    struct Key {
        std::array<std::uint8_t, 16> bytes{};
        bool v6 = false;
        friend bool operator==(const Key&, const Key&) = default;
    };
    [[nodiscard]] static Key key_of(const Endpoint& peer) noexcept;

private:
    struct Entry {
        std::uint32_t connections = 0;
        std::uint64_t milli_tokens = 0;  ///< one token = 1000; refilled lazily from `stamp`
        TimePoint stamp;
    };
    struct KeyHash {
        std::uint64_t seed = 0;
        [[nodiscard]] std::size_t operator()(const Key& key) const noexcept;
    };

    void refill(Entry& entry, TimePoint now) const noexcept;
    [[nodiscard]] bool idle(Entry& entry, TimePoint now) const noexcept;
    void sweep(TimePoint now);

    const IpLimitsConfig config_;
    const std::uint64_t capacity_milli_;  ///< bucket size; 0 when the rate limit is off
    const IClock& clock_;

    mutable std::mutex mutex_;
    std::unordered_map<Key, Entry, KeyHash> table_;  // guarded by mutex_
};

}  // namespace wsrv
