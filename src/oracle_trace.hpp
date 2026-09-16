#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <istream>
#include <stdexcept>

namespace lapsim {

// libCacheSim OracleGeneral records used by the FAST/RAP experiments:
// Python struct format <IQIq = 24 little-endian bytes.
struct OracleRecord {
    std::uint32_t clock_time = 0;
    std::uint64_t object = 0;
    std::uint32_t size = 0;
    std::int64_t next_access_vtime = 0;
};

inline std::uint32_t load_le32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

inline std::uint64_t load_le64(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

inline bool read_oracle_record(std::istream& in, OracleRecord& r) {
    std::array<unsigned char, 24> raw{};
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    const auto got = in.gcount();
    if (got == 0) return false;
    if (got != static_cast<std::streamsize>(raw.size()))
        throw std::runtime_error("truncated 24-byte OracleGeneral record");

    r.clock_time = load_le32(raw.data());
    r.object = load_le64(raw.data() + 4);
    r.size = load_le32(raw.data() + 12);
    const std::uint64_t next_bits = load_le64(raw.data() + 16);
    std::memcpy(&r.next_access_vtime, &next_bits, sizeof(next_bits));
    return true;
}

} // namespace lapsim
