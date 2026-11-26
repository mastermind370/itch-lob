// Zero-copy ITCH 5.0 parser with static dispatch.
//
// Handlers are plain structs; derive from NullHandler and define only the
// callbacks you care about. Calls are resolved at compile time, so an empty
// callback costs nothing and the hot path has no virtual calls.
//
//   struct MyHandler : itch::NullHandler {
//       void on_add(const itch::AddOrder& m) { ... }
//   };
//   MyHandler h;
//   auto r = itch::parse_frames(buf, len, h);
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "itch/endian.hpp"
#include "itch/messages.hpp"

namespace itch {

struct NullHandler {
    void on_system_event(const SystemEvent&) {}
    void on_stock_directory(const StockDirectory&) {}
    void on_add(const AddOrder&) {}
    void on_executed(const OrderExecuted&) {}
    void on_executed_with_price(const OrderExecutedWithPrice&) {}
    void on_cancel(const OrderCancel&) {}
    void on_delete(const OrderDelete&) {}
    void on_replace(const OrderReplace&) {}
    void on_trade(const Trade&) {}
    // Every other known type (H, Y, L, V, W, K, J, h, Q, B, I, N, O) and unknown types.
    void on_other(char /*type*/, const std::uint8_t* /*msg*/, std::size_t /*len*/) {}
};

namespace detail {

[[nodiscard]] inline std::string_view sv(const std::uint8_t* p, std::size_t n) noexcept {
    return {reinterpret_cast<const char*>(p), n};
}

[[nodiscard]] inline Header decode_header(const std::uint8_t* p) noexcept {
    return Header{static_cast<char>(p[0]), load_be16(p + 1), load_be16(p + 3), load_be48(p + 5)};
}

}  // namespace detail

// Decodes one unframed message whose length has already been validated
// (len == kMessageLength[type] for known types).
template <class H>
inline void decode_message(const std::uint8_t* p, std::size_t len, H& h) {
    using namespace detail;
    const char t = static_cast<char>(p[0]);
    switch (t) {
        case type::AddOrder:
        case type::AddOrderMpid:
            h.on_add(AddOrder{decode_header(p), load_be64(p + 11), static_cast<Side>(p[19]), load_be32(p + 20),
                              sv(p + 24, 8), load_be32(p + 32), t == type::AddOrderMpid ? sv(p + 36, 4) : std::string_view{}});
            return;
        case type::OrderDelete:
            h.on_delete(OrderDelete{decode_header(p), load_be64(p + 11)});
            return;
        case type::OrderReplace:
            h.on_replace(OrderReplace{decode_header(p), load_be64(p + 11), load_be64(p + 19), load_be32(p + 27),
                                      load_be32(p + 31)});
            return;
        case type::OrderExecuted:
            h.on_executed(OrderExecuted{decode_header(p), load_be64(p + 11), load_be32(p + 19), load_be64(p + 23)});
            return;
        case type::OrderExecutedPrice:
            h.on_executed_with_price(OrderExecutedWithPrice{decode_header(p), load_be64(p + 11), load_be32(p + 19),
                                                            load_be64(p + 23), static_cast<char>(p[31]),
                                                            load_be32(p + 32)});
            return;
        case type::OrderCancel:
            h.on_cancel(OrderCancel{decode_header(p), load_be64(p + 11), load_be32(p + 19)});
            return;
        case type::Trade:
            h.on_trade(Trade{decode_header(p), load_be64(p + 11), static_cast<Side>(p[19]), load_be32(p + 20),
                             sv(p + 24, 8), load_be32(p + 32), load_be64(p + 36)});
            return;
        case type::SystemEvent:
            h.on_system_event(SystemEvent{decode_header(p), static_cast<char>(p[11])});
            return;
        case type::StockDirectory:
            h.on_stock_directory(StockDirectory{decode_header(p), sv(p + 11, 8), static_cast<char>(p[19]),
                                                static_cast<char>(p[20]), load_be32(p + 21), static_cast<char>(p[25])});
            return;
        default:
            h.on_other(t, p, len);
            return;
    }
}

enum class ParseStatus : std::uint8_t {
    Ok,         // consumed the whole buffer
    Truncated,  // buffer ends mid-frame; resume from `consumed` once more bytes arrive
    BadLength,  // a known message type had the wrong length (corrupt or misaligned input)
};

struct ParseResult {
    std::size_t consumed;    // bytes of complete frames processed
    std::uint64_t messages;  // messages dispatched
    ParseStatus status;
};

// Parses length-prefixed frames as found in NASDAQ's binary ITCH files:
// [u16 big-endian length][message bytes] repeated.
template <class H>
[[nodiscard]] inline ParseResult parse_frames(const std::uint8_t* data, std::size_t len, H& h) {
    std::size_t off = 0;
    std::uint64_t n = 0;
    while (len - off >= 2) {
        const std::size_t mlen = load_be16(data + off);
        if (len - off - 2 < mlen) return {off, n, ParseStatus::Truncated};
        const std::uint8_t* m = data + off + 2;
        if (mlen == 0) return {off, n, ParseStatus::BadLength};
        const std::size_t want = kMessageLength[m[0]];
        if (want != 0 && want != mlen) [[unlikely]]
            return {off, n, ParseStatus::BadLength};
        decode_message(m, mlen, h);
        off += 2 + mlen;
        ++n;
    }
    return {off, n, off == len ? ParseStatus::Ok : ParseStatus::Truncated};
}

}  // namespace itch
