#pragma once

#include "Common.hpp"

#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

inline void mixLevelIntoChecksum(std::uint64_t& checksum,
                                 const std::optional<Level>& level,
                                 std::uint64_t salt) {
    if (!level) {
        checksum ^= salt + 0x9e3779b97f4a7c15ULL + (checksum << 6) + (checksum >> 2);
        return;
    }

    const auto price = static_cast<std::uint64_t>(static_cast<std::int64_t>(level->price));
    const auto qty = static_cast<std::uint64_t>(static_cast<std::int64_t>(level->qty));

    checksum ^= price * salt + 0x9e3779b97f4a7c15ULL + (checksum << 6) + (checksum >> 2);
    checksum ^= qty * (salt | 1ULL) + 0xbf58476d1ce4e5b9ULL + (checksum << 7) + (checksum >> 3);
}

inline void mixBestLevelsIntoChecksum(std::uint64_t& checksum,
                                      const std::optional<Level>& bid,
                                      const std::optional<Level>& ask) {
    mixLevelIntoChecksum(checksum, bid, 1315423911ULL);
    mixLevelIntoChecksum(checksum, ask, 2654435761ULL);
}

inline std::string levelToString(const std::optional<Level>& level) {
    if (!level) {
        return "NA";
    }
    return std::to_string(level->price) + "@" + std::to_string(level->qty);
}

inline std::string checksumToHex(std::uint64_t checksum) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(16) << std::setfill('0') << checksum;
    return out.str();
}
