// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace openurma::proto::net {

inline constexpr std::uint32_t kUdmaWireMagic = 0x31574455U; // "UDW1"

enum class UdmaOperation : std::uint8_t {
    Send = 0,
    SendImmediate = 1,
    Write = 3,
    ReadRequest = 6,
    WriteAck = 0x83,
    ReadResponse = 0x85,
};

struct [[gnu::packed]] UdmaWireHeader {
    std::uint32_t magic;
    std::uint8_t version;
    std::uint8_t operation;
    std::uint16_t flags;
    std::uint32_t source_jetty;
    std::uint32_t destination_jetty;
    std::uint32_t tpn;
    std::uint32_t segment;
    std::uint64_t remote_address;
    std::uint64_t immediate;
    std::uint64_t request_id;
    std::uint32_t payload_length;
};
static_assert(sizeof(UdmaWireHeader) == 52);

} // namespace openurma::proto::net
