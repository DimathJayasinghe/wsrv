// SPDX-License-Identifier: Apache-2.0
#include "server/server_builder.hpp"

#include "net/endpoint.hpp"

#include <string>

namespace wsrv {

namespace {

[[noreturn]] void throw_config_error(const Error& error) {
    std::string message{to_string(error.code)};
    message += ": ";
    message += error.context;
    throw ConfigError{message, error.code};
}

}  // namespace

ServerBuilder& ServerBuilder::listen(std::string_view ip, std::uint16_t port) {
    ip_ = std::string{ip};
    port_ = port;
    return *this;
}

ServerBuilder& ServerBuilder::handler_factory(IHandlerFactory& factory) noexcept {
    deps_.factory = &factory;
    return *this;
}

ServerBuilder& ServerBuilder::logger(ILogger& logger) noexcept {
    deps_.logger = &logger;
    return *this;
}

ServerBuilder& ServerBuilder::clock(const IClock& clock) noexcept {
    deps_.clock = &clock;
    return *this;
}

ServerBuilder& ServerBuilder::syscalls(ISyscalls& sys) noexcept {
    deps_.sys = &sys;
    return *this;
}

std::unique_ptr<Server> ServerBuilder::build() const {
    const Result<Endpoint> endpoint = parse_endpoint(ip_, port_);
    if (!endpoint) {
        throw_config_error(Error{ErrorCode::InvalidConfig, "listen address is not an IP literal"});
    }
    ServerConfig config = config_;
    config.listen = *endpoint;
    Result<std::unique_ptr<Server>> server = Server::create(std::move(config), deps_);
    if (!server) {
        throw_config_error(server.error());
    }
    return std::move(*server);
}

}  // namespace wsrv
