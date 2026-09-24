// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

#include <simbricks/base/proto.h>

// Host-simulator <-> UB-device-simulator protocol. The final 16 bytes of every
// 64-byte header deliberately match SimBricks base protocol: timestamp at byte
// 48 and own_type at byte 63. Payload, when present, follows the header.
namespace openurma::proto::host {

inline constexpr std::uint64_t kProtocolId = 0x5542484f53540001ULL; // UBHOST v1
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kMaxRegions = 8;
inline constexpr std::uint32_t kMaxIrqs = 64;

enum class H2DType : std::uint8_t {
    MmioRead = 0x40,
    MmioWrite = 0x41,
    DmaReadCompletion = 0x42,
    DmaWriteCompletion = 0x43,
    DeviceControl = 0x44,
};

enum class D2HType : std::uint8_t {
    MmioReadCompletion = 0x40,
    DmaRead = 0x41,
    DmaWrite = 0x42,
    Interrupt = 0x43,
};

enum class Status : std::uint16_t {
    Success = 0,
    InvalidAddress = 1,
    InvalidLength = 2,
    PermissionDenied = 3,
    InternalError = 4,
};

enum class AddressKind : std::uint8_t {
    GuestPhysical = 0,
    IoVirtual = 1,
};

enum class InterruptAction : std::uint8_t {
    Lower = 0,
    Raise = 1,
    Pulse = 2,
};

struct DeviceIntro {
    std::uint32_t version;
    std::uint32_t region_count;
    std::uint32_t irq_count;
    std::uint32_t port_count;
    std::uint64_t device_id;
    std::uint64_t feature_bits;
    struct Region {
        std::uint64_t size;
        std::uint32_t type;
        std::uint32_t flags;
    } regions[kMaxRegions];
};

struct HostIntro {
    std::uint32_t version;
    std::uint32_t address_bits;
    std::uint64_t feature_bits;
};

struct [[gnu::packed]] MmioRequest {
    std::uint64_t request_id;
    std::uint64_t offset;
    std::uint64_t value;
    std::uint32_t length;
    std::uint16_t region;
    std::uint16_t byte_enable;
    std::uint8_t reserved[16];
    std::uint64_t timestamp;
    std::uint8_t pad[7];
    std::uint8_t own_type;
};

struct [[gnu::packed]] Completion {
    std::uint64_t request_id;
    std::uint64_t value;
    std::uint32_t length;
    std::uint16_t status;
    std::uint8_t reserved[26];
    std::uint64_t timestamp;
    std::uint8_t pad[7];
    std::uint8_t own_type;
};

struct [[gnu::packed]] DmaRequest {
    std::uint64_t request_id;
    std::uint64_t address;
    std::uint32_t length;
    std::uint32_t pasid;
    std::uint16_t flags;
    std::uint8_t address_kind;
    std::uint8_t reserved0;
    std::uint8_t reserved[20];
    std::uint64_t timestamp;
    std::uint8_t pad[7];
    std::uint8_t own_type;
};

struct [[gnu::packed]] Interrupt {
    std::uint64_t sequence;
    std::uint32_t vector;
    std::uint8_t action;
    std::uint8_t reserved[35];
    std::uint64_t timestamp;
    std::uint8_t pad[7];
    std::uint8_t own_type;
};

union H2DMessage {
    SimbricksProtoBaseMsg base;
    MmioRequest mmio;
    Completion completion;
};

union D2HMessage {
    SimbricksProtoBaseMsg base;
    Completion completion;
    DmaRequest dma;
    Interrupt interrupt;
};

static_assert(sizeof(MmioRequest) == 64);
static_assert(sizeof(Completion) == 64);
static_assert(sizeof(DmaRequest) == 64);
static_assert(sizeof(Interrupt) == 64);
static_assert(sizeof(H2DMessage) == 64);
static_assert(sizeof(D2HMessage) == 64);
static_assert(offsetof(MmioRequest, timestamp) == 48);
static_assert(offsetof(DmaRequest, timestamp) == 48);

} // namespace openurma::proto::host
