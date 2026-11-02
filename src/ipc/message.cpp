// SPDX-License-Identifier: Apache-2.0
#include "ipc/message.hpp"

#include "base/checked.hpp"

#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace wsrv::ipc {

namespace {

constexpr std::size_t kHelloFixed = 6;   // major, minor, token length
constexpr std::size_t kHelloOkSize = 8;  // major, minor, max frame
constexpr std::size_t kSendFixed = 12;   // request, connection
constexpr std::size_t kCloseFixed = 14;  // request, connection, code
constexpr std::size_t kEventFixed = 12;  // kind, connection, close code, clean
constexpr std::size_t kStatusSize = 5;   // request, status

[[nodiscard]] bool known_kind(EventKind kind) noexcept {
    const auto raw = static_cast<std::uint8_t>(kind);
    return raw >= static_cast<std::uint8_t>(EventKind::Open) &&
           raw <= static_cast<std::uint8_t>(EventKind::Error);
}

/// The shape every Event must have, on both sides of the wire: a real connection, a known kind, and
/// a close code / clean flag only on Close events.
[[nodiscard]] bool event_fields_consistent(EventKind kind,
                                           std::uint64_t conn,
                                           std::uint16_t close_code,
                                           bool clean) noexcept {
    if (conn == 0 || !known_kind(kind)) {
        return false;
    }
    return kind == EventKind::Close || (close_code == 0 && !clean);
}

// ---- writing -----------------------------------------------------------------------------------

class Writer {
public:
    explicit Writer(std::vector<std::byte>& out) noexcept : out_{out} {}
    void u8(std::uint8_t v) {
        out_.push_back(static_cast<std::byte>(v));
    }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v >> 8U));
        u8(static_cast<std::uint8_t>(v & 0xFFU));
    }
    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v >> 16U));
        u16(static_cast<std::uint16_t>(v & 0xFFFFU));
    }
    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v >> 32U));
        u32(static_cast<std::uint32_t>(v & 0xFFFFFFFFU));
    }
    void bytes(ByteView v) {
        out_.insert(out_.end(), v.begin(), v.end());
    }

private:
    std::vector<std::byte>& out_;
};

struct Encoded {
    FrameType type;
    std::size_t body_size;
};

/// Type and body size of `message`, or InvalidArgument when no peer could parse it.
[[nodiscard]] Result<Encoded> describe(const Message& message) {
    struct Visitor {
        Result<Encoded> operator()(const Hello& m) const {
            if (m.token.size() > kMaxTokenSize) {
                return fail(ErrorCode::InvalidArgument, "ipc: token too long");
            }
            return Encoded{FrameType::Hello, kHelloFixed + m.token.size()};
        }
        Result<Encoded> operator()(const HelloOk& /*m*/) const {
            return Encoded{FrameType::HelloOk, kHelloOkSize};
        }
        Result<Encoded> operator()(const SendData& m) const {
            if (m.conn == 0) {
                return fail(ErrorCode::InvalidArgument, "ipc: connection id 0");
            }
            return Encoded{m.text ? FrameType::SendText : FrameType::SendBinary,
                           kSendFixed + m.data.size()};
        }
        Result<Encoded> operator()(const CloseCommand& m) const {
            if (m.conn == 0 || m.reason.size() > kMaxCloseReason) {
                return fail(ErrorCode::InvalidArgument, "ipc: bad close command");
            }
            return Encoded{FrameType::Close, kCloseFixed + m.reason.size()};
        }
        Result<Encoded> operator()(const Ping& /*m*/) const {
            return Encoded{FrameType::Ping, 0};
        }
        Result<Encoded> operator()(const Pong& /*m*/) const {
            return Encoded{FrameType::Pong, 0};
        }
        Result<Encoded> operator()(const Event& m) const {
            if (!event_fields_consistent(m.kind, m.conn, m.close_code, m.clean)) {
                return fail(ErrorCode::InvalidArgument, "ipc: inconsistent event");
            }
            return Encoded{FrameType::Event, kEventFixed + m.data.size()};
        }
        Result<Encoded> operator()(const Status& /*m*/) const {
            return Encoded{FrameType::Status, kStatusSize};
        }
    };
    return std::visit(Visitor{}, message);
}

void write_body(Writer& w, const Message& message) {
    struct Visitor {
        Writer& w;
        void operator()(const Hello& m) const {
            w.u16(m.major);
            w.u16(m.minor);
            w.u16(static_cast<std::uint16_t>(m.token.size()));
            w.bytes(m.token);
        }
        void operator()(const HelloOk& m) const {
            w.u16(m.major);
            w.u16(m.minor);
            w.u32(m.max_frame);
        }
        void operator()(const SendData& m) const {
            w.u32(m.request);
            w.u64(m.conn);
            w.bytes(m.data);
        }
        void operator()(const CloseCommand& m) const {
            w.u32(m.request);
            w.u64(m.conn);
            w.u16(m.code);
            w.bytes(m.reason);
        }
        void operator()(const Ping& /*m*/) const {}
        void operator()(const Pong& /*m*/) const {}
        void operator()(const Event& m) const {
            w.u8(static_cast<std::uint8_t>(m.kind));
            w.u64(m.conn);
            w.u16(m.close_code);
            w.u8(m.clean ? 1 : 0);
            w.bytes(m.data);
        }
        void operator()(const Status& m) const {
            w.u32(m.request);
            w.u8(m.status);
        }
    };
    std::visit(Visitor{w}, message);
}

// ---- reading -----------------------------------------------------------------------------------

/// Reads big-endian integers from a body; every read is bounds-checked by the caller through
/// remaining(), and a violation can only mean a size check above was forgotten (asserted).
class Reader {
public:
    explicit Reader(ByteView body) noexcept : rest_{body} {}
    [[nodiscard]] std::size_t remaining() const noexcept {
        return rest_.size();
    }
    std::uint8_t u8() {
        WSRV_ASSERT(rest_.size() >= 1);
        const auto v = std::to_integer<std::uint8_t>(rest_[0]);
        rest_ = rest_.subspan(1);
        return v;
    }
    std::uint16_t u16() {
        WSRV_ASSERT(rest_.size() >= 2);
        const std::uint16_t v = load_be16(rest_);
        rest_ = rest_.subspan(2);
        return v;
    }
    std::uint32_t u32() {
        WSRV_ASSERT(rest_.size() >= 4);
        const std::uint32_t v = load_be32(rest_);
        rest_ = rest_.subspan(4);
        return v;
    }
    std::uint64_t u64() {
        WSRV_ASSERT(rest_.size() >= 8);
        const std::uint64_t v = load_be64(rest_);
        rest_ = rest_.subspan(8);
        return v;
    }
    /// Everything that is left.
    ByteView take_rest() noexcept {
        const ByteView all = rest_;
        rest_ = ByteView{};
        return all;
    }
    ByteView take(std::size_t count) {
        WSRV_ASSERT(rest_.size() >= count);
        const ByteView head = rest_.first(count);
        rest_ = rest_.subspan(count);
        return head;
    }

private:
    ByteView rest_;
};

std::unexpected<Error> invalid(std::string_view why) noexcept {
    return fail(ErrorCode::InvalidFrame, why);
}

}  // namespace

std::optional<FrameType> frame_type_from(std::uint8_t raw) noexcept {
    switch (static_cast<FrameType>(raw)) {
    case FrameType::Hello:
    case FrameType::HelloOk:
    case FrameType::SendText:
    case FrameType::SendBinary:
    case FrameType::Close:
    case FrameType::Ping:
    case FrameType::Pong:
    case FrameType::Event:
    case FrameType::Status:
        return static_cast<FrameType>(raw);
    }
    return std::nullopt;
}

Result<void> encode(const Message& message, std::size_t max_frame, ByteBuffer& out) {
    const Result<Encoded> shape = describe(message);
    if (!shape) {
        return std::unexpected{shape.error()};
    }
    const std::size_t length = 2 + shape->body_size;  // type + flags + body
    if (length > max_frame || length > kMaxFrameCeiling) {
        return fail(ErrorCode::BufferFull, "ipc: frame above the limit");
    }
    try {
        // Built whole in a scratch vector so that a failure half-way leaves `out` untouched.
        std::vector<std::byte> frame;
        frame.reserve(kFrameHeaderSize + shape->body_size);
        Writer w{frame};
        w.u32(static_cast<std::uint32_t>(length));
        w.u8(static_cast<std::uint8_t>(shape->type));
        w.u8(0);
        write_body(w, message);
        return out.append(ByteView{frame});
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "ipc: out of memory while encoding");
    }
}

Result<Message> parse_message(FrameType type, ByteView body) {
    Reader r{body};
    switch (type) {
    case FrameType::Hello: {
        if (r.remaining() < kHelloFixed) {
            return invalid("ipc: hello too short");
        }
        Hello m;
        m.major = r.u16();
        m.minor = r.u16();
        const std::size_t token_size = r.u16();
        if (token_size > kMaxTokenSize || r.remaining() != token_size) {
            return invalid("ipc: hello token size");
        }
        m.token = r.take(token_size);
        return m;
    }
    case FrameType::HelloOk: {
        if (r.remaining() != kHelloOkSize) {
            return invalid("ipc: hello_ok size");
        }
        HelloOk m;
        m.major = r.u16();
        m.minor = r.u16();
        m.max_frame = r.u32();
        return m;
    }
    case FrameType::SendText:
    case FrameType::SendBinary: {
        if (r.remaining() < kSendFixed) {
            return invalid("ipc: send too short");
        }
        SendData m;
        m.text = type == FrameType::SendText;
        m.request = r.u32();
        m.conn = r.u64();
        if (m.conn == 0) {
            return invalid("ipc: connection id 0");
        }
        m.data = r.take_rest();
        return m;
    }
    case FrameType::Close: {
        if (r.remaining() < kCloseFixed) {
            return invalid("ipc: close too short");
        }
        CloseCommand m;
        m.request = r.u32();
        m.conn = r.u64();
        m.code = r.u16();
        if (m.conn == 0 || r.remaining() > kMaxCloseReason) {
            return invalid("ipc: close command fields");
        }
        m.reason = r.take_rest();
        return m;
    }
    case FrameType::Ping:
    case FrameType::Pong: {
        if (r.remaining() != 0) {
            return invalid("ipc: ping/pong carries no body");
        }
        if (type == FrameType::Ping) {
            return Message{Ping{}};
        }
        return Message{Pong{}};
    }
    case FrameType::Event: {
        if (r.remaining() < kEventFixed) {
            return invalid("ipc: event too short");
        }
        Event m;
        m.kind = static_cast<EventKind>(r.u8());
        m.conn = r.u64();
        m.close_code = r.u16();
        const std::uint8_t clean = r.u8();
        if (clean > 1) {
            return invalid("ipc: clean flag");
        }
        m.clean = clean == 1;
        if (!event_fields_consistent(m.kind, m.conn, m.close_code, m.clean)) {
            return invalid("ipc: inconsistent event");
        }
        m.data = r.take_rest();
        return m;
    }
    case FrameType::Status: {
        if (r.remaining() != kStatusSize) {
            return invalid("ipc: status size");
        }
        Status m;
        m.request = r.u32();
        m.status = r.u8();
        return m;
    }
    }
    return invalid("ipc: unknown frame type");
}

}  // namespace wsrv::ipc
