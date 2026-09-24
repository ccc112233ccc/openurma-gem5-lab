// SPDX-License-Identifier: Apache-2.0
#include "openurma/udma_model.h"

#include <cstring>
#include <utility>

namespace openurma::device {

namespace {
constexpr std::uint8_t kOpcodeSend = 1;
constexpr std::uint32_t kStatusReady = 1U;
constexpr std::uint32_t kStatusSuccess = 0U;
constexpr std::uint32_t kStatusDmaError = 1U;
constexpr std::uint32_t kInterruptVector = 0U;
}

UdmaModel::UdmaModel(HostInterface& host, NetworkInterface& network)
    : host_(host), network_(network)
{
}

std::uint64_t
UdmaModel::ReadMmio(std::uint64_t offset, std::uint32_t length) const
{
    if (length != sizeof(std::uint64_t)) {
        return 0;
    }
    if (offset == kRegisterIdentity) {
        return kIdentity;
    }
    if (offset == kRegisterStatus) {
        return kStatusReady;
    }
    return 0;
}

bool
UdmaModel::WriteMmio(std::uint64_t offset, std::uint32_t length,
                     std::uint64_t value)
{
    if (offset != kRegisterDoorbell || length != sizeof(value) || value == 0) {
        return false;
    }
    FetchDescriptor(value);
    return true;
}

void
UdmaModel::FetchDescriptor(std::uint64_t address)
{
    host_.DmaRead(address, sizeof(Descriptor),
        [this](bool ok, std::vector<std::uint8_t> bytes) {
            if (!ok || bytes.size() != sizeof(Descriptor)) {
                return;
            }
            Descriptor descriptor{};
            std::memcpy(&descriptor, bytes.data(), sizeof(descriptor));
            const std::uint64_t sequence = next_sequence_++;
            ++submitted_;
            if (descriptor.opcode != kOpcodeSend || descriptor.length == 0) {
                Finish(sequence, descriptor, kStatusDmaError);
                return;
            }
            pending_.emplace(sequence, descriptor);
            FetchPayload(sequence, descriptor);
        });
}

void
UdmaModel::FetchPayload(std::uint64_t sequence, Descriptor descriptor)
{
    host_.DmaRead(descriptor.payload_address, descriptor.length,
        [this, sequence, descriptor](bool ok, std::vector<std::uint8_t> bytes) mutable {
            if (!ok || bytes.size() != descriptor.length) {
                Finish(sequence, descriptor, kStatusDmaError);
                return;
            }
            Frame frame{};
            frame.sequence = sequence;
            frame.source_eid = descriptor.source_eid;
            frame.destination_eid = descriptor.destination_eid;
            frame.source_port = descriptor.source_port;
            frame.destination_port = descriptor.source_port;
            frame.bytes = std::move(bytes);
            network_.Send(std::move(frame),
                [this, sequence, descriptor](bool sent) {
                    Finish(sequence, descriptor,
                           sent ? kStatusSuccess : kStatusDmaError);
                });
        });
}

void
UdmaModel::Finish(std::uint64_t sequence, const Descriptor& descriptor,
                  std::uint32_t status)
{
    CompletionEntry entry{sequence, status, descriptor.length};
    const auto* first = reinterpret_cast<const std::uint8_t*>(&entry);
    host_.DmaWrite(descriptor.completion_address,
                   std::vector<std::uint8_t>(first, first + sizeof(entry)),
        [this, sequence](bool ok) {
            pending_.erase(sequence);
            if (!ok) {
                return;
            }
            ++completed_;
            host_.SetInterrupt(kInterruptVector, true);
        });
}

void
UdmaModel::Receive(Frame frame)
{
    (void)frame;
    // Receive queues are deliberately a separate extraction milestone. Keeping
    // this entry point in the stable core API prevents the host adapters from
    // acquiring receive-side device semantics in the meantime.
}

} // namespace openurma::device
