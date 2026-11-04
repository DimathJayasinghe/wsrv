// SPDX-License-Identifier: Apache-2.0
#include "protocol/permessage_deflate.hpp"

#include <string_view>

namespace wsrv {
namespace {

constexpr std::string_view kExtensionName = "permessage-deflate";
constexpr std::string_view kServerNoContext = "server_no_context_takeover";
constexpr std::string_view kClientNoContext = "client_no_context_takeover";
constexpr std::string_view kServerWindow = "server_max_window_bits";
constexpr std::string_view kClientWindow = "client_max_window_bits";

// RFC 7692 section 7.1.2: the value is 8..15 written as plain decimal digits. Strict: no sign, no
// whitespace, no leading zero ("08" is not what the RFC's grammar describes as a number).
std::optional<int> parse_window_bits(std::string_view text) {
    if (text.empty() || text.size() > 2) {
        return std::nullopt;
    }
    if (text.front() < '1' || text.front() > '9') {  // also rejects a leading '0'
        return std::nullopt;
    }
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    if (value < 8 || value > kMaxDeflateWindowBits) {
        return std::nullopt;
    }
    return value;
}

int smaller(int a, int b) noexcept {
    return a < b ? a : b;
}

std::string
render_answer(const DeflateParams& params, bool server_window_offered, bool client_window_offered) {
    std::string out{kExtensionName};
    // The server's own restrictions are announced even when the client did not ask for them
    // (section 7.1.1.1 allows server_no_context_takeover in a response regardless).
    if (params.server_no_context_takeover) {
        out += "; ";
        out += kServerNoContext;
    }
    if (params.client_no_context_takeover) {
        out += "; ";
        out += kClientNoContext;
    }
    if (server_window_offered || params.server_window_bits < kMaxDeflateWindowBits) {
        out += "; ";
        out += kServerWindow;
        out += '=';
        out += std::to_string(params.server_window_bits);
    }
    // Section 7.1.2.2: only a client that offered client_max_window_bits may be answered with it.
    if (client_window_offered) {
        out += "; ";
        out += kClientWindow;
        out += '=';
        out += std::to_string(params.client_window_bits);
    }
    return out;
}

std::optional<DeflateAgreement> accept_offer(const ExtensionOffer& offer,
                                             const DeflatePolicy& policy) {
    DeflateParams params;
    params.server_no_context_takeover = policy.server_no_context_takeover;
    params.server_window_bits = policy.server_max_window_bits;
    bool server_window_offered = false;
    bool client_window_offered = false;

    // The offer parser has already rejected duplicate parameter names inside one offer.
    for (const auto& [name, value] : offer.params) {
        if (name == kServerNoContext) {
            if (!value.empty()) {
                return std::nullopt;
            }
            params.server_no_context_takeover = true;
        } else if (name == kClientNoContext) {
            if (!value.empty()) {
                return std::nullopt;
            }
            params.client_no_context_takeover = true;
        } else if (name == kServerWindow) {
            // A value is required (section 7.1.2.1). 8 is valid on the wire but not for us.
            const auto bits = parse_window_bits(value);
            if (!bits || *bits < kMinDeflateWindowBits) {
                return std::nullopt;
            }
            params.server_window_bits = smaller(*bits, policy.server_max_window_bits);
            server_window_offered = true;
        } else if (name == kClientWindow) {
            // The value is optional (section 7.1.2.2); without one the client accepts any bound.
            int offered = kMaxDeflateWindowBits;
            if (!value.empty()) {
                const auto bits = parse_window_bits(value);
                if (!bits) {
                    return std::nullopt;
                }
                offered = *bits;
            }
            params.client_window_bits = smaller(offered, policy.client_max_window_bits);
            client_window_offered = true;
        } else {
            return std::nullopt;  // unknown parameter: this offer cannot be accepted (section 5.2)
        }
    }
    return DeflateAgreement{params,
                            render_answer(params, server_window_offered, client_window_offered)};
}

}  // namespace

bool valid(const DeflatePolicy& policy) noexcept {
    const auto in_range = [](int bits) {
        return bits >= kMinDeflateWindowBits && bits <= kMaxDeflateWindowBits;
    };
    return in_range(policy.server_max_window_bits) && in_range(policy.client_max_window_bits);
}

std::optional<DeflateAgreement> negotiate_permessage_deflate(std::span<const ExtensionOffer> offers,
                                                             const DeflatePolicy& policy) {
    for (const ExtensionOffer& offer : offers) {
        if (offer.name != kExtensionName) {
            continue;
        }
        if (auto agreement = accept_offer(offer, policy)) {
            return agreement;
        }
    }
    return std::nullopt;
}

}  // namespace wsrv
