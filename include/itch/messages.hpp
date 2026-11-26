// NASDAQ TotalView-ITCH 5.0 message definitions.
//
// Decoded structs hold integers by value and string fields as std::string_view
// pointing into the original buffer, so decoding never copies or allocates.
// The buffer must outlive any decoded message that you keep.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace itch {

// Prices are fixed point with 4 implied decimals ($12.3400 -> 123400).
using Price = std::uint32_t;
inline constexpr std::uint32_t kPriceScale = 10'000;
[[nodiscard]] constexpr double price_to_double(Price p) noexcept { return static_cast<double>(p) / kPriceScale; }

namespace type {
inline constexpr char SystemEvent = 'S';
inline constexpr char StockDirectory = 'R';
inline constexpr char TradingAction = 'H';
inline constexpr char RegSho = 'Y';
inline constexpr char MarketParticipantPos = 'L';
inline constexpr char MwcbDecline = 'V';
inline constexpr char MwcbStatus = 'W';
inline constexpr char IpoQuoting = 'K';
inline constexpr char LuldCollar = 'J';
inline constexpr char OperationalHalt = 'h';
inline constexpr char AddOrder = 'A';
inline constexpr char AddOrderMpid = 'F';
inline constexpr char OrderExecuted = 'E';
inline constexpr char OrderExecutedPrice = 'C';
inline constexpr char OrderCancel = 'X';
inline constexpr char OrderDelete = 'D';
inline constexpr char OrderReplace = 'U';
inline constexpr char Trade = 'P';
inline constexpr char CrossTrade = 'Q';
inline constexpr char BrokenTrade = 'B';
inline constexpr char Noii = 'I';
inline constexpr char Rpii = 'N';
inline constexpr char Dlcr = 'O';
}  // namespace type

// Exact on-wire length (excluding the 2-byte frame length) for each type.
// 0 = unknown type.
inline constexpr std::array<std::uint8_t, 256> kMessageLength = [] {
    std::array<std::uint8_t, 256> t{};
    t['S'] = 12; t['R'] = 39; t['H'] = 25; t['Y'] = 20; t['L'] = 26;
    t['V'] = 35; t['W'] = 12; t['K'] = 28; t['J'] = 35; t['h'] = 21;
    t['A'] = 36; t['F'] = 40; t['E'] = 31; t['C'] = 36; t['X'] = 23;
    t['D'] = 19; t['U'] = 35; t['P'] = 44; t['Q'] = 40; t['B'] = 19;
    t['I'] = 50; t['N'] = 20; t['O'] = 48;
    return t;
}();

inline constexpr std::size_t kHeaderLength = 11;  // type, locate, tracking, timestamp

enum class Side : char { Buy = 'B', Sell = 'S' };

struct Header {
    char type;
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    std::uint64_t timestamp;  // ns since midnight
};

struct SystemEvent {
    Header hdr;
    char event_code;  // 'O','S','Q','M','E','C'
};

struct StockDirectory {
    Header hdr;
    std::string_view stock;  // 8 bytes, space padded
    char market_category;
    char financial_status;
    std::uint32_t round_lot_size;
    char round_lots_only;
};

// 'A' and 'F'. attribution is empty for 'A'.
struct AddOrder {
    Header hdr;
    std::uint64_t order_ref;
    Side side;
    std::uint32_t shares;
    std::string_view stock;
    Price price;
    std::string_view attribution;
};

struct OrderExecuted {
    Header hdr;
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
};

struct OrderExecutedWithPrice {
    Header hdr;
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
    char printable;
    Price execution_price;
};

struct OrderCancel {
    Header hdr;
    std::uint64_t order_ref;
    std::uint32_t cancelled_shares;
};

struct OrderDelete {
    Header hdr;
    std::uint64_t order_ref;
};

// Replace = delete original + add new_order_ref at (shares, price) on the same side/stock.
struct OrderReplace {
    Header hdr;
    std::uint64_t original_order_ref;
    std::uint64_t new_order_ref;
    std::uint32_t shares;
    Price price;
};

// Non-cross trade against a hidden order; does not change the visible book.
struct Trade {
    Header hdr;
    std::uint64_t order_ref;
    Side side;
    std::uint32_t shares;
    std::string_view stock;
    Price price;
    std::uint64_t match_number;
};

// Trims the space padding from an 8-byte ITCH alpha field.
[[nodiscard]] constexpr std::string_view trim_symbol(std::string_view s) noexcept {
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

}  // namespace itch
