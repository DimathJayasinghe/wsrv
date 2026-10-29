// SPDX-License-Identifier: Apache-2.0
#include "server/session_handle.hpp"

#include <new>
#include <string>
#include <utility>
#include <vector>

namespace wsrv {

Result<void> LoopLink::submit(ConnectionId id, SessionCommand command) {
    const std::lock_guard lock{mutex_};
    if (loop_ == nullptr) {
        return fail(ErrorCode::Closing, "session handle: the server is gone");
    }
    ISessionDirectory* const directory = directory_;
    try {
        return loop_->post(
            [directory, id, command = std::move(command)] { directory->run(id, command); });
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "session handle: out of memory");
    }
}

void LoopLink::close() noexcept {
    const std::lock_guard lock{mutex_};
    loop_ = nullptr;
    directory_ = nullptr;
}

namespace {

[[nodiscard]] Result<void>
submit_to(const std::shared_ptr<LoopLink>& link, ConnectionId id, SessionCommand command) {
    if (link == nullptr) {
        return fail(ErrorCode::Closing, "session handle: not connected");
    }
    return link->submit(id, std::move(command));
}

}  // namespace

Result<void> SessionHandle::send_text(std::string_view text) const {
    try {
        return submit_to(link_, id_, [copy = std::string{text}](Session& session) {
            static_cast<void>(session.send_text(copy).has_value());  // policy applies inside
        });
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "session handle: out of memory");
    }
}

Result<void> SessionHandle::send_binary(ByteView data) const {
    try {
        return submit_to(
            link_,
            id_,
            [copy = std::vector<std::byte>(data.begin(), data.end())](Session& session) {
                static_cast<void>(session.send_binary(ByteView{copy}).has_value());
            });
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "session handle: out of memory");
    }
}

Result<void> SessionHandle::close(CloseCode code, std::string_view reason) const {
    try {
        return submit_to(link_, id_, [code, copy = std::string{reason}](Session& session) {
            session.close(code, copy);
        });
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "session handle: out of memory");
    }
}

}  // namespace wsrv
