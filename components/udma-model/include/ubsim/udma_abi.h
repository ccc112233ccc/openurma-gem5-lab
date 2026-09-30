// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>

namespace ubsim::device::abi {

inline constexpr std::uint32_t kHardwarePageBytes = 4096;
inline constexpr std::uint32_t kWqebbBytes = 64;
inline constexpr std::uint32_t kCqeBytes = 64;
inline constexpr std::uint32_t kSgeBytes = 16;
inline constexpr std::uint32_t kDoorbellOffset = 0x80;
inline constexpr std::uint64_t kUdmaMemoryOffset = 0x00200000;

inline std::uint32_t Load32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
        (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

inline std::uint64_t Load64(const std::uint8_t* p)
{
    return Load32(p) | (std::uint64_t(Load32(p + 4)) << 32);
}

inline void Store32(std::uint8_t* p, std::uint32_t value)
{
    for (std::uint32_t i = 0; i < 4; ++i)
        p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

inline void Store64(std::uint8_t* p, std::uint64_t value)
{
    Store32(p, static_cast<std::uint32_t>(value));
    Store32(p + 4, static_cast<std::uint32_t>(value >> 32));
}

struct Sge {
    std::uint32_t length{};
    std::uint32_t token{};
    std::uint64_t address{};
};

class SqWqe {
  public:
    explicit SqWqe(const std::array<std::uint8_t, kWqebbBytes>& raw)
        : bytes_(raw) {}
    std::uint16_t index() const { return Load32(bytes_.data()) & 0xffffU; }
    std::uint8_t flags() const { return (Load32(bytes_.data()) >> 16) & 0x7fU; }
    bool completion() const { return flags() & (1U << 5); }
    bool inline_payload() const { return flags() & (1U << 6); }
    bool owner() const { return Load32(bytes_.data()) >> 31; }
    std::uint8_t opcode() const { return (Load32(bytes_.data() + 4) >> 8) & 0xffU; }
    std::uint16_t inline_length() const { return Load32(bytes_.data() + 4) >> 22; }
    std::uint32_t tpn() const { return Load32(bytes_.data() + 8) & 0xffffffU; }
    std::uint8_t sge_count() const { return Load32(bytes_.data() + 8) >> 24; }
    std::uint32_t remote_jetty() const { return Load32(bytes_.data() + 12) & 0xfffffU; }
    std::uint32_t remote_eid() const { return Load32(bytes_.data() + 16) & 0xfffffU; }
    std::uint32_t remote_segment() const { return remote_jetty(); }
    std::uint64_t remote_address() const { return Load64(bytes_.data() + 40); }
    std::uint64_t immediate() const { return Load64(bytes_.data() + 40); }
    Sge first_sge() const
    {
        return {Load32(bytes_.data() + 48), Load32(bytes_.data() + 52),
                Load64(bytes_.data() + 56)};
    }
    std::uint32_t wqebb_count() const
    {
        if (inline_payload())
            return (48U + inline_length() + kWqebbBytes - 1) / kWqebbBytes;
        return (48U + (sge_count() ? sge_count() - 1U : 0U) * kSgeBytes) /
            kWqebbBytes + 1U;
    }
  private:
    std::array<std::uint8_t, kWqebbBytes> bytes_;
};

inline std::array<std::uint8_t, kCqeBytes>
MakeCqe(bool receive, bool jetty, bool owner, std::uint8_t opcode,
        std::uint16_t entry, std::uint32_t local_id, std::uint32_t bytes,
        std::uint64_t user_data, std::uint64_t immediate,
        std::uint32_t remote_id = 0, std::uint32_t remote_eid = 0,
        std::uint32_t tpn = 0)
{
    std::array<std::uint8_t, kCqeBytes> cqe{};
    Store32(cqe.data(), (receive ? 1U : 0U) | (jetty ? 2U : 0U) |
        (owner ? 4U : 0U) | (std::uint32_t(opcode & 7U) << 4));
    Store32(cqe.data() + 4, entry | ((local_id & 0xffffU) << 16));
    Store32(cqe.data() + 8, ((local_id >> 16) & 0xfU) |
        ((remote_id & 0xfffffU) << 4));
    Store32(cqe.data() + 12, tpn & 0xffffffU);
    Store32(cqe.data() + 16, bytes);
    Store64(cqe.data() + 20, user_data);
    Store32(cqe.data() + 28, remote_eid & 0xfffffU);
    Store64(cqe.data() + 44, immediate);
    return cqe;
}

} // namespace ubsim::device::abi
