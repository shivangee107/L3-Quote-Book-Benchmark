#pragma once

#include "Common.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

using OrderId = std::uint64_t;
using TradeId = std::uint64_t;

struct L3Message {
    char op_code{};       // 'N' = new, 'X' = cancel, 'M' = modify, 'T' = trade
    Side side{};          // order side for N/X/M; aggressor side for T
    std::string symbol;   // fixed 8-byte field on the wire
    std::uint64_t seq_num{};
    std::uint64_t send_timestamp_ns{};
    OrderId order_id{};
    Price price_tick{};
    Qty quantity{};
    TradeId trade_id{};   // only used for T
};

namespace l3wire {

inline constexpr std::size_t kMessageSize = 64;
inline constexpr std::size_t kOpOffset = 0;
inline constexpr std::size_t kSideOffset = 1;
inline constexpr std::size_t kSymbolOffset = 2;
inline constexpr std::size_t kSymbolSize = 8;
inline constexpr std::size_t kSeqOffset = 16;
inline constexpr std::size_t kTimestampOffset = 24;
inline constexpr std::size_t kOrderIdOffset = 32;
inline constexpr std::size_t kPriceOffset = 40;
inline constexpr std::size_t kQtyOffset = 44;
inline constexpr std::size_t kTradeIdOffset = 48;

inline char encodeSideChar(Side side) {
    return side == Side::Bid ? 'B' : 'S';
}

inline std::optional<Side> decodeSideChar(char side) {
    if (side == 'B') {
        return Side::Bid;
    }
    if (side == 'S') {
        return Side::Ask;
    }
    return std::nullopt;
}

inline bool isValidOpCode(char op) {
    return op == 'N' || op == 'X' || op == 'M' || op == 'T';
}

template <typename T>
inline void writeScalar(std::array<char, kMessageSize>& out, std::size_t offset, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(out.data() + offset, &value, sizeof(T));
}

template <typename T>
inline T readScalar(const char* data, std::size_t offset) {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    std::memcpy(&value, data + offset, sizeof(T));
    return value;
}

inline std::array<char, kMessageSize> serialize(const L3Message& msg) {
    std::array<char, kMessageSize> out{};
    out[kOpOffset] = msg.op_code;
    out[kSideOffset] = encodeSideChar(msg.side);

    const std::string symbol = msg.symbol.empty() ? "FOO" : msg.symbol;
    const std::size_t copy_len = std::min(symbol.size(), kSymbolSize);
    std::memcpy(out.data() + kSymbolOffset, symbol.data(), copy_len);

    writeScalar<std::uint64_t>(out, kSeqOffset, msg.seq_num);
    writeScalar<std::uint64_t>(out, kTimestampOffset, msg.send_timestamp_ns);
    writeScalar<OrderId>(out, kOrderIdOffset, msg.order_id);
    writeScalar<Price>(out, kPriceOffset, msg.price_tick);
    writeScalar<Qty>(out, kQtyOffset, msg.quantity);
    writeScalar<TradeId>(out, kTradeIdOffset, msg.trade_id);
    return out;
}

inline std::string parseSymbol(const char* data) {
    const char* begin = data + kSymbolOffset;
    std::size_t len = 0;
    while (len < kSymbolSize && begin[len] != '\0') {
        ++len;
    }
    return std::string(begin, begin + len);
}

inline std::optional<L3Message> parseOne(const char* data, std::size_t size) {
    if (size < kMessageSize) {
        return std::nullopt;
    }

    const char op = data[kOpOffset];
    if (!isValidOpCode(op)) {
        return std::nullopt;
    }

    const auto side = decodeSideChar(data[kSideOffset]);
    if (!side) {
        return std::nullopt;
    }

    L3Message msg{};
    msg.op_code = op;
    msg.side = *side;
    msg.symbol = parseSymbol(data);
    msg.seq_num = readScalar<std::uint64_t>(data, kSeqOffset);
    msg.send_timestamp_ns = readScalar<std::uint64_t>(data, kTimestampOffset);
    msg.order_id = readScalar<OrderId>(data, kOrderIdOffset);
    msg.price_tick = readScalar<Price>(data, kPriceOffset);
    msg.quantity = readScalar<Qty>(data, kQtyOffset);
    msg.trade_id = readScalar<TradeId>(data, kTradeIdOffset);

    if (msg.symbol.empty()) {
        return std::nullopt;
    }

    if (msg.op_code == 'N' || msg.op_code == 'M' || msg.op_code == 'T') {
        if (msg.price_tick < kDefaultMinPriceTick || msg.price_tick > kDefaultMaxPriceTick) {
            return std::nullopt;
        }
        if (msg.quantity <= 0) {
            return std::nullopt;
        }
    }

    if (msg.op_code == 'N' || msg.op_code == 'X' || msg.op_code == 'M') {
        if (msg.order_id == 0) {
            return std::nullopt;
        }
    }

    return msg;
}

inline void appendSerialized(std::vector<char>& stream, const L3Message& msg) {
    const auto encoded = serialize(msg);
    stream.insert(stream.end(), encoded.begin(), encoded.end());
}

} // namespace l3wire
