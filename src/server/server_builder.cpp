// SPDX-License-Identifier: Apache-2.0
#include "server/server_builder.hpp"

#include "net/endpoint.hpp"

#if WSRV_HAS_DEFLATE
#include "extensions/zlib_codec.hpp"
#endif

#include <memory>
#include <string>

namespace wsrv {

namespace {

[[noreturn]] void throw_config_error(const Error& error) {
    std::string message{to_string(error.code)};
    message += ": ";
    message += error.context;
    throw ConfigError{message, error.code};
}

/// ADR-0020: put the deflate policy and the zlib codecs into `config` together, or refuse. The
/// pairing is also checked by validate(ServerConfig); doing it here makes the half-configured
/// state unrepresentable for everyone who goes through the builder.
void apply_deflate([[maybe_unused]] const DeflateOptions& options,
                   [[maybe_unused]] ServerConfig& config) {
#if WSRV_HAS_DEFLATE
    const ZlibSettings settings{options.level, options.memory_level};
    if (!valid(settings)) {
        throw_config_error(Error{ErrorCode::InvalidConfig,
                                 "permessage_deflate: level and memory_level must be 1..9"});
    }
    SessionDeflate deflate;
    deflate.codecs = std::make_shared<ZlibCodecFactory>(settings);
    deflate.min_compress_size = options.min_compress_size;
    deflate.max_inflate_ratio = options.max_inflate_ratio;
    config.handshake.permessage_deflate = options.policy;
    config.session.deflate =
        deflate;  // the agreed parameters replace deflate.params per connection
#else
    throw_config_error(
        Error{ErrorCode::InvalidConfig,
              "permessage_deflate: this build has no zlib (WSRV_ENABLE_DEFLATE=OFF)"});
#endif
}

}  // namespace

ServerBuilder& ServerBuilder::listen(std::string_view ip, std::uint16_t port) {
    ip_ = std::string{ip};
    port_ = port;
    return *this;
}

ServerBuilder&
ServerBuilder::publish(std::string_view ip, std::uint16_t port, const PublishSources& sources) {
    publish_ = std::make_pair(std::string{ip}, port);
    deps_.publish = &sources;
    return *this;
}

ServerBuilder& ServerBuilder::admin(std::string_view ip, std::uint16_t port) {
    admin_ = std::make_pair(std::string{ip}, port);
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

ServerBuilder& ServerBuilder::transport_wrapper(ITransportWrapper& wrapper) noexcept {
    deps_.transport_wrapper = &wrapper;
    return *this;
}

ServerBuilder& ServerBuilder::handshake_decider(IHandshakeDecider& decider) noexcept {
    deps_.decider = &decider;
    return *this;
}

ServerBuilder& ServerBuilder::permessage_deflate(const DeflateOptions& options) {
    deflate_ = options;
    return *this;
}

std::unique_ptr<Server> ServerBuilder::build() const {
    const Result<Endpoint> endpoint = parse_endpoint(ip_, port_);
    if (!endpoint) {
        throw_config_error(Error{ErrorCode::InvalidConfig, "listen address is not an IP literal"});
    }
    ServerConfig config = config_;
    config.listen = *endpoint;
    if (admin_) {
        const Result<Endpoint> admin = parse_endpoint(admin_->first, admin_->second);
        if (!admin) {
            throw_config_error(
                Error{ErrorCode::InvalidConfig, "admin address is not an IP literal"});
        }
        config.admin = *admin;
    }
    if (publish_) {
        const Result<Endpoint> publish = parse_endpoint(publish_->first, publish_->second);
        if (!publish) {
            throw_config_error(
                Error{ErrorCode::InvalidConfig, "publish address is not an IP literal"});
        }
        config.publish = *publish;
    }
    if (deflate_) {
        apply_deflate(*deflate_, config);
    }
    Result<std::unique_ptr<Server>> server = Server::create(std::move(config), deps_);
    if (!server) {
        throw_config_error(server.error());
    }
    return std::move(*server);
}

}  // namespace wsrv
