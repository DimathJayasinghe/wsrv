// SPDX-License-Identifier: Apache-2.0
#include "server/ip_limits.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <new>

namespace wsrv {

namespace {

constexpr std::uint64_t kMilliPerToken = 1000;
/// Highest rate honoured; ServerConfig validation refuses more, this is the belt to its braces.
constexpr std::uint32_t kMaxRate = 1'000'000;
constexpr std::chrono::milliseconds kFullAfter{1000};  // a bucket is full after a second of silence

/// splitmix64 finaliser: a cheap, well-mixed 64-bit permutation.
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::uint64_t load64(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8U) | bytes[i];
    }
    return value;
}

}  // namespace

IpLimits::IpLimits(const IpLimitsConfig& config, const IClock& clock)
    : config_{config},
      capacity_milli_{
          static_cast<std::uint64_t>(std::min(config.max_new_connections_per_second, kMaxRate)) *
          kMilliPerToken},
      clock_{clock}, table_{0, KeyHash{config.hash_seed}} {}

IpLimits::Key IpLimits::key_of(const Endpoint& peer) noexcept {
    Key key;
    if (!peer.v6) {
        std::copy_n(peer.address.begin(), 4, key.bytes.begin());
        return key;
    }
    const auto& a = peer.address;
    const bool mapped =
        std::all_of(a.begin(), a.begin() + 10, [](std::uint8_t b) { return b == 0; }) &&
        a[10] == 0xFF && a[11] == 0xFF;
    if (mapped) {  // ::ffff:a.b.c.d is the IPv4 client a.b.c.d
        std::copy_n(a.begin() + 12, 4, key.bytes.begin());
        return key;
    }
    key.v6 = true;
    std::copy_n(a.begin(), 8, key.bytes.begin());  // the /64 prefix; the rest stays zero
    return key;
}

std::size_t IpLimits::KeyHash::operator()(const Key& key) const noexcept {
    const std::uint64_t high = load64(key.bytes.data());
    const std::uint64_t low = load64(key.bytes.data() + 8);
    return static_cast<std::size_t>(mix(mix(high ^ seed) ^ low ^ (key.v6 ? 1U : 0U)));
}

/// Add the tokens earned since `stamp`. Only whole milliseconds are consumed, so a caller that
/// asks every half millisecond still earns its tokens. Time going backwards earns nothing.
void IpLimits::refill(Entry& entry, TimePoint now) const noexcept {
    if (capacity_milli_ == 0 || now <= entry.stamp) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.stamp);
    if (elapsed >= kFullAfter) {  // clamped first: elapsed * rate cannot overflow below
        entry.milli_tokens = capacity_milli_;
        entry.stamp = now;
        return;
    }
    // elapsed < 1000 ms and the rate <= kMaxRate: the product is below 1e9, no overflow possible.
    const auto earned =
        static_cast<std::uint64_t>(elapsed.count()) * (capacity_milli_ / kMilliPerToken);
    entry.milli_tokens = std::min(capacity_milli_, entry.milli_tokens + earned);
    entry.stamp += elapsed;
}

/// True when the entry remembers nothing worth keeping: no connection and a full bucket.
bool IpLimits::idle(Entry& entry, TimePoint now) const noexcept {
    if (entry.connections != 0) {
        return false;
    }
    refill(entry, now);
    return capacity_milli_ == 0 || entry.milli_tokens >= capacity_milli_;
}

void IpLimits::sweep(TimePoint now) {
    for (auto it = table_.begin(); it != table_.end();) {
        it = idle(it->second, now) ? table_.erase(it) : std::next(it);
    }
}

Admission IpLimits::admit(const Endpoint& peer) {
    const Key key = key_of(peer);
    const TimePoint now = clock_.now();
    const std::lock_guard lock{mutex_};
    try {
        auto found = table_.find(key);
        if (found == table_.end()) {
            if (table_.size() >= config_.max_tracked) {
                sweep(now);
            }
            if (table_.size() >= config_.max_tracked) {
                return Admission::TableFull;  // shed instead of growing (ADR-0021 section 3)
            }
            found = table_.emplace(key, Entry{0, capacity_milli_, now}).first;  // a full bucket
        }
        Entry& entry = found->second;
        refill(entry, now);
        if (config_.max_connections_per_ip != 0 &&
            entry.connections >= config_.max_connections_per_ip) {
            return Admission::TooManyConnections;  // refusals cost no token
        }
        if (capacity_milli_ != 0) {
            if (entry.milli_tokens < kMilliPerToken) {
                return Admission::RateExceeded;
            }
            entry.milli_tokens -= kMilliPerToken;
        }
        ++entry.connections;
        return Admission::Admitted;
    } catch (const std::bad_alloc&) {
        return Admission::TableFull;  // cannot remember the address: refuse, never admit untracked
    }
}

void IpLimits::release(const Endpoint& peer) noexcept {
    const Key key = key_of(peer);
    const TimePoint now = clock_.now();
    const std::lock_guard lock{mutex_};
    const auto found = table_.find(key);
    if (found == table_.end() || found->second.connections == 0) {
        return;
    }
    --found->second.connections;
    if (idle(found->second, now)) {
        table_.erase(found);  // an idle server holds no per-address state
    }
}

std::size_t IpLimits::tracked() const {
    const std::lock_guard lock{mutex_};
    return table_.size();
}

}  // namespace wsrv
