// SPDX-License-Identifier: Apache-2.0
#include "http/request.hpp"

#include "http/token.hpp"

namespace wsrv {

std::vector<std::string_view> HttpRequest::values(std::string_view name) const {
    std::vector<std::string_view> out;
    for (const HttpHeader& header : headers) {
        if (iequals(header.name, name)) {
            out.emplace_back(header.value);
        }
    }
    return out;
}

std::size_t HttpRequest::count(std::string_view name) const {
    std::size_t total = 0;
    for (const HttpHeader& header : headers) {
        if (iequals(header.name, name)) {
            ++total;
        }
    }
    return total;
}

}  // namespace wsrv
