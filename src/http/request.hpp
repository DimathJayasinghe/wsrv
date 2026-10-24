// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wsrv {

/// One header field. `name` is kept as received; compare it case-insensitively (HttpRequest does).
struct HttpHeader {
    std::string name;
    std::string value;
};

/// A parsed HTTP/1.x request head (no body). Owns its strings: a handshake is parsed once per
/// connection and is bounded (REQUIREMENTS section 4), so copies are cheap and nothing dangles.
/// Thread-affinity: a plain value; not synchronised.
struct HttpRequest {
    std::string method;
    std::string target;
    int version_minor = 1;  // HTTP/1.<version_minor>
    std::vector<HttpHeader> headers;

    /// All values of header `name` (ASCII case-insensitive), in arrival order. Views are valid
    /// while this request is alive and unmodified.
    [[nodiscard]] std::vector<std::string_view> values(std::string_view name) const;

    /// Number of header fields called `name` (duplicates counted).
    [[nodiscard]] std::size_t count(std::string_view name) const;
};

}  // namespace wsrv
