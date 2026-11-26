// Builds framed ITCH 5.0 messages. Used by tests and the synthetic feed
// generator; not on any hot path.
#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "itch/endian.hpp"
#include "itch/messages.hpp"

namespace itch {

class Encoder {
public:
    explicit Encoder(std::vector<std::uint8_t>& out) : out_(out) {}

    void system_event(std::uint16_t locate, std::uint64_t ts, char code) {
        auto* p = begin(type::SystemEvent, locate, ts);
        p[11] = static_cast<std::uint8_t>(code);
    }

    void stock_directory(std::uint16_t locate, std::uint64_t ts, std::string_view stock,
                         std::uint32_t round_lot = 100) {
        auto* p = begin(type::StockDirectory, locate, ts);
        put_alpha(p + 11, stock, 8);
        p[19] = 'Q';  // market category: NASDAQ Global Select
        p[20] = 'N';  // financial status: normal
        store_be32(p + 21, round_lot);
        p[25] = 'N';
        p[26] = 'C';
        p[27] = ' '; p[28] = ' ';
        p[29] = 'P';
        p[30] = 'N';
        p[31] = ' ';
        p[32] = '1';
        p[33] = 'N';
        store_be32(p + 34, 0);
        p[38] = 'N';
    }

    void add_order(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, Side side, std::uint32_t shares,
                   std::string_view stock, Price price, std::string_view mpid = {}) {
        auto* p = begin(mpid.empty() ? type::AddOrder : type::AddOrderMpid, locate, ts);
        store_be64(p + 11, ref);
        p[19] = static_cast<std::uint8_t>(side);
        store_be32(p + 20, shares);
        put_alpha(p + 24, stock, 8);
        store_be32(p + 32, price);
        if (!mpid.empty()) put_alpha(p + 36, mpid, 4);
    }

    void executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares,
                  std::uint64_t match) {
        auto* p = begin(type::OrderExecuted, locate, ts);
        store_be64(p + 11, ref);
        store_be32(p + 19, shares);
        store_be64(p + 23, match);
    }

    void executed_with_price(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares,
                             std::uint64_t match, char printable, Price price) {
        auto* p = begin(type::OrderExecutedPrice, locate, ts);
        store_be64(p + 11, ref);
        store_be32(p + 19, shares);
        store_be64(p + 23, match);
        p[31] = static_cast<std::uint8_t>(printable);
        store_be32(p + 32, price);
    }

    void cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
        auto* p = begin(type::OrderCancel, locate, ts);
        store_be64(p + 11, ref);
        store_be32(p + 19, shares);
    }

    void del(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
        auto* p = begin(type::OrderDelete, locate, ts);
        store_be64(p + 11, ref);
    }

    void replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t old_ref, std::uint64_t new_ref,
                 std::uint32_t shares, Price price) {
        auto* p = begin(type::OrderReplace, locate, ts);
        store_be64(p + 11, old_ref);
        store_be64(p + 19, new_ref);
        store_be32(p + 27, shares);
        store_be32(p + 31, price);
    }

    void trade(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, Side side, std::uint32_t shares,
               std::string_view stock, Price price, std::uint64_t match) {
        auto* p = begin(type::Trade, locate, ts);
        store_be64(p + 11, ref);
        p[19] = static_cast<std::uint8_t>(side);
        store_be32(p + 20, shares);
        put_alpha(p + 24, stock, 8);
        store_be32(p + 32, price);
        store_be64(p + 36, match);
    }

    // Arbitrary raw frame, for testing malformed or unknown input.
    void raw(const std::vector<std::uint8_t>& msg) {
        const std::size_t at = out_.size();
        out_.resize(at + 2 + msg.size());
        store_be16(out_.data() + at, static_cast<std::uint16_t>(msg.size()));
        std::memcpy(out_.data() + at + 2, msg.data(), msg.size());
    }

private:
    std::uint8_t* begin(char t, std::uint16_t locate, std::uint64_t ts) {
        const std::size_t len = kMessageLength[static_cast<std::uint8_t>(t)];
        const std::size_t at = out_.size();
        out_.resize(at + 2 + len, 0);
        std::uint8_t* f = out_.data() + at;
        store_be16(f, static_cast<std::uint16_t>(len));
        std::uint8_t* p = f + 2;
        p[0] = static_cast<std::uint8_t>(t);
        store_be16(p + 1, locate);
        store_be16(p + 3, 0);  // tracking number
        store_be48(p + 5, ts);
        return p;
    }

    static void put_alpha(std::uint8_t* p, std::string_view s, std::size_t width) {
        std::memset(p, ' ', width);
        std::memcpy(p, s.data(), s.size() < width ? s.size() : width);
    }

    std::vector<std::uint8_t>& out_;
};

}  // namespace itch
