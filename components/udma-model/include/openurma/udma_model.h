// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace openurma::device {

using Completion = std::function<void(bool)>;
using ReadCompletion = std::function<void(bool, std::vector<std::uint8_t>)>;

class HostInterface {
  public:
    virtual ~HostInterface() = default;
    virtual void DmaRead(std::uint64_t address, std::size_t length,
                         ReadCompletion completion) = 0;
    virtual void DmaWrite(std::uint64_t address,
                          std::vector<std::uint8_t> data,
                          Completion completion) = 0;
    virtual void SetInterrupt(std::uint32_t vector, bool asserted) = 0;
};

struct Frame {
    std::uint64_t sequence{};
    std::uint32_t source_eid{};
    std::uint32_t destination_eid{};
    std::uint16_t source_port{};
    std::uint16_t destination_port{};
    std::vector<std::uint8_t> bytes;
};

class NetworkInterface {
  public:
    virtual ~NetworkInterface() = default;
    virtual void Send(Frame frame, Completion completion) = 0;
};

// First simulator-independent execution slice. The descriptor format is an
// internal model contract used only by the extraction tests; official UDMA WQE
// decoders will replace/extend it as they move out of NICTopologySC.
struct Descriptor {
    std::uint8_t opcode;
    std::uint8_t source_port;
    std::uint16_t flags;
    std::uint32_t length;
    std::uint32_t source_eid;
    std::uint32_t destination_eid;
    std::uint64_t payload_address;
    std::uint64_t completion_address;
};
static_assert(sizeof(Descriptor) == 32);

struct CompletionEntry {
    std::uint64_t sequence;
    std::uint32_t status;
    std::uint32_t bytes;
};
static_assert(sizeof(CompletionEntry) == 16);

class UdmaModel {
  public:
    static constexpr std::uint64_t kRegisterIdentity = 0x0000;
    static constexpr std::uint64_t kRegisterStatus = 0x0008;
    static constexpr std::uint64_t kRegisterDoorbell = 0x0100;
    static constexpr std::uint64_t kIdentity = 0x4f50454e55444d41ULL;

    UdmaModel(HostInterface& host, NetworkInterface& network);

    std::uint64_t ReadMmio(std::uint64_t offset, std::uint32_t length) const;
    bool WriteMmio(std::uint64_t offset, std::uint32_t length,
                   std::uint64_t value);
    void Receive(Frame frame);

    std::uint64_t submitted() const { return submitted_; }
    std::uint64_t completed() const { return completed_; }

  private:
    void FetchDescriptor(std::uint64_t address);
    void FetchPayload(std::uint64_t sequence, Descriptor descriptor);
    void Finish(std::uint64_t sequence, const Descriptor& descriptor,
                std::uint32_t status);

    HostInterface& host_;
    NetworkInterface& network_;
    std::uint64_t next_sequence_{1};
    std::uint64_t submitted_{0};
    std::uint64_t completed_{0};
    std::unordered_map<std::uint64_t, Descriptor> pending_;
};

} // namespace openurma::device
