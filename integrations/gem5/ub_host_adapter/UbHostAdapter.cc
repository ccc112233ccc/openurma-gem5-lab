// SPDX-License-Identifier: Apache-2.0
#include "ub_host_adapter/UbHostAdapter.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "mem/packet_access.hh"
#include "sim/sim_exit.hh"

#include <algorithm>
#include <cstring>
#include <thread>

namespace gem5
{

namespace host_proto = openurma::proto::host;

namespace
{

template <typename T>
void
zeroVolatile(volatile T &object)
{
    auto *bytes = reinterpret_cast<volatile uint8_t *>(&object);
    for (size_t i = 0; i < sizeof(T); ++i)
        bytes[i] = 0;
}

} // anonymous namespace

UbHostAdapter::DmaOperation::DmaOperation(
    UbHostAdapter &owner, uint64_t request_id, bool is_read, size_t length)
    : owner(owner), requestId(request_id), read(is_read), bytes(length),
      done([this] { this->owner.completeDma(this); },
           owner.name() + ".dmaDone")
{
}

UbHostAdapter::UbHostAdapter(const Params &params)
    : DmaDevice(params), pioAddr(params.pio_addr), pioSize(params.pio_size),
      pioLatency(params.pio_latency), pollInterval(params.poll_interval),
      socketPath(params.socket_path), interrupt(params.interrupt->get()),
      pollEvent([this] { pollDevice(); }, name() + ".poll")
{
}

UbHostAdapter::~UbHostAdapter()
{
    if (interface.base.conn_state != 0)
        SimbricksBaseIfClose(&interface.base);
}

void
UbHostAdapter::init()
{
    DmaDevice::init();
    connectDevice();
    schedule(pollEvent, curTick() + pollInterval);
}

AddrRangeList
UbHostAdapter::getAddrRanges() const
{
    return {RangeSize(pioAddr, pioSize)};
}

uint64_t
UbHostAdapter::protocolTime() const
{
    // gem5's default tick is one picosecond, matching the SimBricks ABI.
    return curTick();
}

void
UbHostAdapter::connectDevice()
{
    SimbricksBaseIfParams params{};
    host_proto::DefaultParams(&params);
    params.sock_path = socketPath.c_str();
    // Functional bring-up deliberately does not constrain gem5's event queue.
    // Synchronization will be enabled by a later event-boundary integration,
    // never by short periodic vCPU exits.
    params.sync_mode = kSimbricksBaseIfSyncDisabled;
    if (SimbricksBaseIfInit(&interface.base, &params) != 0 ||
        SimbricksBaseIfConnect(&interface.base) != 0)
        fatal("%s: cannot connect UB-HOST socket %s\n", name(), socketPath);

    host_proto::HostIntro host_intro{host_proto::kVersion, 64, 0};
    SimBricksBaseIfEstablishData establish{
        &interface.base, &host_intro, sizeof(host_intro),
        &deviceIntro, sizeof(deviceIntro)};
    if (SimBricksBaseIfEstablish(&establish, 1) != 0 ||
        deviceIntro.version != host_proto::kVersion)
        fatal("%s: UB-HOST handshake failed\n", name());
}

Tick
UbHostAdapter::read(PacketPtr packet)
{
    const Addr offset = packet->getAddr() - pioAddr;
    const uint64_t value = transactMmio(offset, packet->getSize(), 0, false);
    packet->setUintX(value, ByteOrder::little);
    packet->makeResponse();
    return pioLatency;
}

Tick
UbHostAdapter::write(PacketPtr packet)
{
    const Addr offset = packet->getAddr() - pioAddr;
    transactMmio(offset, packet->getSize(),
                 packet->getUintX(ByteOrder::little), true);
    packet->makeResponse();
    return pioLatency;
}

uint64_t
UbHostAdapter::transactMmio(Addr offset, unsigned length, uint64_t value,
                            bool write)
{
    auto *message = host_proto::UbHostH2DOutAlloc(&interface, protocolTime());
    if (!message)
        fatal("%s: UB-HOST request ring is full\n", name());
    zeroVolatile(message->mmio);
    const uint64_t request_id = nextRequest++;
    message->mmio.request_id = request_id;
    message->mmio.offset = offset;
    message->mmio.length = length;
    message->mmio.value = value;
    host_proto::UbHostH2DOutSend(
        &interface, message,
        static_cast<uint8_t>(write ? host_proto::H2DType::MmioWrite
                                   : host_proto::H2DType::MmioRead));

    // PioDevice's atomic API is synchronous. Keep servicing device-originated
    // requests while waiting, so a future register transaction may itself
    // trigger DMA without deadlocking the two processes.
    while (mmioCompletions.find(request_id) == mmioCompletions.end()) {
        pollDevice();
        std::this_thread::yield();
    }
    const uint64_t result = mmioCompletions.at(request_id);
    mmioCompletions.erase(request_id);
    return result;
}

void
UbHostAdapter::pollDevice()
{
    while (auto *message =
               host_proto::UbHostD2HInPoll(&interface, UINT64_MAX)) {
        const auto type = static_cast<host_proto::D2HType>(
            host_proto::UbHostD2HInType(&interface, message));
        switch (type) {
          case host_proto::D2HType::MmioCompletion:
            if (message->completion.status !=
                static_cast<uint16_t>(host_proto::Status::Success))
                fatal("%s: device rejected MMIO request %llu\n", name(),
                      static_cast<unsigned long long>(
                          message->completion.request_id));
            mmioCompletions.emplace(message->completion.request_id,
                                    message->completion.value);
            break;
          case host_proto::D2HType::DmaRead:
            handleDma(message, true);
            break;
          case host_proto::D2HType::DmaWrite:
            handleDma(message, false);
            break;
          case host_proto::D2HType::Interrupt:
            handleInterrupt(message->interrupt);
            break;
        }
        host_proto::UbHostD2HInDone(&interface, message);
    }
    dmaOperations.erase(
        std::remove_if(dmaOperations.begin(), dmaOperations.end(),
            [](const auto &operation) { return operation->completed; }),
        dmaOperations.end());
    if (!pollEvent.scheduled())
        schedule(pollEvent, curTick() + pollInterval);
}

void
UbHostAdapter::handleDma(volatile host_proto::D2HMessage *message, bool read)
{
    auto owned_operation = std::make_unique<DmaOperation>(
        *this, message->dma.request_id, read, message->dma.length);
    auto *operation = owned_operation.get();
    dmaOperations.push_back(std::move(owned_operation));
    if (!read) {
        const auto *source = reinterpret_cast<volatile uint8_t *>(message) +
                             sizeof(host_proto::D2HMessage);
        for (size_t i = 0; i < operation->bytes.size(); ++i)
            operation->bytes[i] = source[i];
        dmaWrite(message->dma.address, operation->bytes.size(),
                 &operation->done, operation->bytes.data());
    } else {
        dmaRead(message->dma.address, operation->bytes.size(),
                &operation->done, operation->bytes.data());
    }
}

void
UbHostAdapter::completeDma(DmaOperation *operation)
{
    sendDmaCompletion(*operation, true);
    operation->completed = true;
}

void
UbHostAdapter::sendDmaCompletion(const DmaOperation &operation, bool success)
{
    auto *message = host_proto::UbHostH2DOutAlloc(&interface, protocolTime());
    if (!message)
        fatal("%s: UB-HOST completion ring is full\n", name());
    zeroVolatile(message->completion);
    message->completion.request_id = operation.requestId;
    message->completion.length = operation.bytes.size();
    message->completion.status = static_cast<uint16_t>(
        success ? host_proto::Status::Success : host_proto::Status::InternalError);
    if (operation.read) {
        auto *destination = reinterpret_cast<volatile uint8_t *>(message) +
                            sizeof(host_proto::H2DMessage);
        for (size_t i = 0; i < operation.bytes.size(); ++i)
            destination[i] = operation.bytes[i];
    }
    host_proto::UbHostH2DOutSend(
        &interface, message,
        static_cast<uint8_t>(operation.read
            ? host_proto::H2DType::DmaReadCompletion
            : host_proto::H2DType::DmaWriteCompletion));
}

void
UbHostAdapter::handleInterrupt(
    const volatile host_proto::Interrupt &interrupt_message)
{
    if (!interrupt)
        return;
    const auto action = static_cast<host_proto::InterruptAction>(
        interrupt_message.action);
    if (action == host_proto::InterruptAction::Lower)
        interrupt->clear();
    else
        interrupt->raise();
}

} // namespace gem5
