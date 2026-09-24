// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

#include <simbricks/base/proto.h>

// UB endpoint <-> fabric boundary. Payload is a wire-visible UB frame or flit,
// never a WQE or an internal RMA transaction.
namespace openurma::proto::net {

inline constexpr std::uint64_t kProtocolId = 0x55424e4554000001ULL; // UBNET v1
inline constexpr std::uint32_t kVersion = 1;

enum class MessageType : std::uint8_t {
    Frame = 0x40,
    LinkState = 0x41,
    Credit = 0x42,
};

enum class LinkState : std::uint8_t {
    Down = 0,
    Up = 1,
};

struct Intro {
    std::uint32_t version;
    std::uint32_t port_count;
    std::uint32_t max_frame_bytes;
    std::uint32_t flit_bytes;
    std::uint64_t feature_bits;
};

struct [[gnu::packed]] Frame {
    std::uint64_t sequence;
    std::uint32_t length;
    std::uint32_t source_eid;
    std::uint32_t destination_eid;
    std::uint16_t source_port;
    std::uint16_t destination_port;
    std::uint16_t traffic_class;
    std::uint16_t flags;
    std::uint8_t reserved[20];
    std::uint64_t timestamp;
    std::uint8_t pad[7];
    std::uint8_t own_type;
};

struct [[gnu::packed]] LinkControl {
    std::uint64_t sequence;
    std::uint32_t value;
    std::uint16_t port;
    std::uint8_t state;
    std::uint8_t reserved[33];
    std::uint64_t timestamp;
    std::uint8_t pad[7];
    std::uint8_t own_type;
};

union Message {
    SimbricksProtoBaseMsg base;
    Frame frame;
    LinkControl link;
};

static_assert(sizeof(Frame) == 64);
static_assert(sizeof(LinkControl) == 64);
static_assert(sizeof(Message) == 64);
static_assert(offsetof(Frame, timestamp) == 48);

} // namespace openurma::proto::net
