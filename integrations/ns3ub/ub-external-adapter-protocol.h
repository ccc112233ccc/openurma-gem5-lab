// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_EXTERNAL_ADAPTER_PROTOCOL_H
#define UB_EXTERNAL_ADAPTER_PROTOCOL_H

#include <cstddef>
#include <cstdint>

namespace ns3::ub_external
{

constexpr uint64_t MAGIC = 0x4f55554241445054ULL;
constexpr uint32_t VERSION = 4;
constexpr uint32_t RING_SLOTS = 64;
constexpr uint32_t SLOT_BYTES = 8192;
constexpr uint32_t MAX_PORTS = 16;

enum class MessageType : uint32_t
{
    DATA = 1,
    SYNC = 2,
    LINK_STATE = 3,
};

struct alignas(64) RingIndex
{
    uint64_t value;
    uint8_t padding[56];
};

struct alignas(64) MessageHeader
{
    uint32_t payloadLength;
    uint32_t type;
    uint64_t sendTick;
    uint64_t receiveTick;
    uint64_t sequence;
    uint16_t sourcePort;
    uint16_t destinationPort;
    uint16_t protocolVersion;
    uint16_t flags;
    uint32_t sourceEid;
    uint32_t destinationEid;
    uint64_t syncReserved;
    uint8_t reserved[8];
};

struct alignas(4096) Ring
{
    struct Direction
    {
        RingIndex head;
        RingIndex tail;
    } direction[2][MAX_PORTS];

    uint64_t endpointSyncMessages;
    uint64_t switchSyncMessages;
    uint32_t localEid;
    uint32_t controlReserved32;
    uint8_t controlReserved[4072];
};

static_assert(sizeof(RingIndex) == 64);
static_assert(sizeof(MessageHeader) == 64);
static_assert(sizeof(Ring) == 8192);

inline constexpr std::size_t
MappedBytes(uint32_t ports)
{
    return sizeof(Ring) + 2ULL * ports * RING_SLOTS * SLOT_BYTES;
}

inline constexpr std::size_t
SlotOffset(uint32_t ports, uint32_t direction, uint32_t port, uint64_t index)
{
    const std::size_t queue = static_cast<std::size_t>(direction) * ports + port;
    return sizeof(Ring) + (queue * RING_SLOTS + index % RING_SLOTS) * SLOT_BYTES;
}

} // namespace ns3::ub_external

#endif // UB_EXTERNAL_ADAPTER_PROTOCOL_H
