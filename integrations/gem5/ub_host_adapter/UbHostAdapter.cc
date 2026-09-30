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

namespace host_proto = ubsim::proto::host;

namespace
{

template <typename T>
void
zeroVolatile(volatile T &object)
{
    const uint64_t timestamp = object.timestamp;
    auto *bytes = reinterpret_cast<volatile uint8_t *>(&object);
    for (size_t i = 0; i < sizeof(T); ++i)
        bytes[i] = 0;
    object.timestamp = timestamp;
}

} // anonymous namespace

UbHostAdapter::DmaOperation::DmaOperation(
    UbHostAdapter &owner, uint64_t request_id, bool is_read, bool is_msi,
    size_t length)
    : owner(owner), requestId(request_id), read(is_read), msi(is_msi),
      bytes(length),
      done([this] { this->owner.completeDma(this); },
           owner.name() + ".dmaDone")
{
}

UbHostAdapter::UbHostAdapter(const Params &params)
    : DmaDevice(params), pioAddr(params.pio_addr), pioSize(params.pio_size),
      pioLatency(params.pio_latency), pollInterval(params.poll_interval),
      syncEnabled(params.sync), lifecycleSync(params.lifecycle_sync),
      linkLatency(params.link_latency),
      syncInterval(params.sync_interval),
      socketPath(params.socket_path), msiPort(this, sys),
      interrupts{params.interrupt_misc ? params.interrupt_misc->get() : nullptr,
                 params.interrupt_aeq ? params.interrupt_aeq->get() : nullptr,
                 params.interrupt_ceq ? params.interrupt_ceq->get() : nullptr},
      pollEvent([this] { pollDevice(); }, name() + ".poll")
{
}

UbHostAdapter::~UbHostAdapter()
{
    const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - wallStarted).count();
    inform("[UB_HOST_PROFILE] object=%s wall_ns=%lld poll_events=%llu "
           "service_polls=%llu empty_service_polls=%llu sync_messages=%llu "
           "sync_backpressure=%llu boundary_wait_yields=%llu mmio=%llu dma=%llu\n",
           name(), static_cast<long long>(wall_ns),
           static_cast<unsigned long long>(pollEvents),
           static_cast<unsigned long long>(servicePolls),
           static_cast<unsigned long long>(emptyServicePolls),
           static_cast<unsigned long long>(syncMessages),
           static_cast<unsigned long long>(syncBackpressure),
           static_cast<unsigned long long>(boundaryWaitYields),
           static_cast<unsigned long long>(mmioTransactions),
           static_cast<unsigned long long>(dmaTransactions));
    if (interface.base.conn_state != 0)
        SimbricksBaseIfClose(&interface.base);
}

void
UbHostAdapter::init()
{
    DmaDevice::init();
    connectDevice();
    if (host_proto::UbHostH2DOutSync(&interface, protocolTime()) == 0 &&
        SimbricksBaseIfSyncEnabled(&interface.base))
        ++syncMessages;
}

void
UbHostAdapter::startup()
{
    DmaDevice::startup();
    // init() runs before a checkpoint restores curTick.  Scheduling there
    // leaves a boot-time poll event in the past when a late shell snapshot is
    // loaded.  startup() runs after checkpoint state and the event queue have
    // been restored, so the first external-device poll is always relative to
    // the active epoch.
    scheduleNextPoll();
}

AddrRangeList
UbHostAdapter::getAddrRanges() const
{
    return {RangeSize(pioAddr, pioSize)};
}

Port &
UbHostAdapter::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "msi")
        return msiPort;
    return DmaDevice::getPort(if_name, idx);
}

uint64_t
UbHostAdapter::protocolTime() const
{
    // gem5's default tick is one picosecond, matching the SimBricks ABI.
    const uint64_t absolute = std::max<uint64_t>(curTick(), serviceTime);
    return lifecycleSyncActive ? absolute - epochOrigin : absolute;
}

void
UbHostAdapter::connectDevice()
{
    if (syncEnabled && syncInterval > linkLatency)
        fatal("%s: UB-HOST sync interval must not exceed link latency\n",
              name());
    if (syncEnabled && pioLatency < 2 * linkLatency)
        fatal("%s: atomic PIO latency must cover the UB-HOST round trip\n",
              name());
    SimbricksBaseIfParams params{};
    host_proto::DefaultParams(&params);
    params.sock_path = socketPath.c_str();
    params.link_latency = linkLatency;
    params.sync_interval = syncInterval;
    params.sync_mode = syncEnabled ? kSimbricksBaseIfSyncRequired
                                   : kSimbricksBaseIfSyncDisabled;
    if (SimbricksBaseIfInit(&interface.base, &params) != 0 ||
        SimbricksBaseIfConnect(&interface.base) != 0)
        fatal("%s: cannot connect UB-HOST socket %s\n", name(), socketPath);

    host_proto::HostIntro host_intro{
        host_proto::kVersion, 64, 0, pioAddr, pioSize};
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
    ++mmioTransactions;
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
    const uint64_t deadline = protocolTime() + pioLatency;
    while (mmioCompletions.find(request_id) == mmioCompletions.end()) {
        // Atomic PIO cannot return control to gem5's event queue before the
        // response exists.  Treat the request/response pair as one atomic
        // transaction, but never consume a device message beyond the latency
        // gem5 will charge for this access.
        serviceDevice(deadline);
        std::this_thread::yield();
    }
    const uint64_t result = mmioCompletions.at(request_id);
    mmioCompletions.erase(request_id);
    return result;
}

void
UbHostAdapter::pollDevice()
{
    ++pollEvents;
    serviceDevice(protocolTime());
    while (host_proto::UbHostH2DOutSync(&interface, protocolTime()) != 0) {
        ++syncBackpressure;
        std::this_thread::yield();
    }
    if (SimbricksBaseIfSyncEnabled(&interface.base))
        ++syncMessages;
    scheduleNextPoll();
}

bool
UbHostAdapter::serviceDevice(uint64_t deadline)
{
    ++servicePolls;
    bool progress = false;
    while (auto *message = host_proto::UbHostD2HInPoll(&interface, deadline)) {
        progress = true;
        const uint64_t message_time = message->base.header.timestamp;
        const uint64_t absolute_message_time = lifecycleSyncActive
            ? epochOrigin + message_time : message_time;
        serviceTime = std::max<uint64_t>(curTick(), absolute_message_time);
        const auto type = static_cast<host_proto::D2HType>(
            host_proto::UbHostD2HInType(&interface, message));
        switch (type) {
          case host_proto::D2HType::MmioCompletion:
          {
            // The wire ABI is packed and volatile.  Copy fields before using
            // them as STL arguments; those APIs require ordinary references
            // which cannot bind directly to packed members.
            const uint64_t request_id = message->completion.request_id;
            const uint64_t value = message->completion.value;
            if (message->completion.status !=
                static_cast<uint16_t>(host_proto::Status::Success))
                fatal("%s: device rejected MMIO request %llu\n", name(),
                      static_cast<unsigned long long>(request_id));
            mmioCompletions.emplace(request_id, value);
            break;
          }
          case host_proto::D2HType::DmaRead:
            handleDma(message, true);
            break;
          case host_proto::D2HType::DmaWrite:
            handleDma(message, false);
            break;
          case host_proto::D2HType::Interrupt:
            handleInterrupt(message->interrupt);
            break;
          case host_proto::D2HType::Lifecycle:
            if (message->lifecycle.action == static_cast<uint8_t>(
                    host_proto::LifecycleAction::CommitSync) &&
                message->lifecycle.generation == lifecycleGeneration) {
                lifecycleCommitSeen = true;
                lifecycleCommitEnabled = message->lifecycle.enabled != 0;
            }
            break;
        }
        host_proto::UbHostD2HInDone(&interface, message);
        serviceTime = 0;
    }
    dmaOperations.erase(
        std::remove_if(dmaOperations.begin(), dmaOperations.end(),
            [](const auto &operation) { return operation->completed; }),
        dmaOperations.end());
    if (!progress)
        ++emptyServicePolls;
    return progress;
}

void
UbHostAdapter::scheduleNextPoll()
{
    if (pollEvent.scheduled())
        return;
    Tick next = curTick() + pollInterval;
    if (SimbricksBaseIfSyncEnabled(&interface.base)) {
        // At a conservative horizon gem5 must not advance even one tick until
        // the device publishes a newer SYNC/data timestamp.  Wait at this
        // simulator boundary (not in the vCPU) and then schedule directly at
        // the newly granted horizon.
        while (!SimbricksBaseIfInTerminated(&interface.base)) {
            auto *visible = host_proto::UbHostD2HInPeek(&interface, UINT64_MAX);
            if (visible != nullptr) {
                const uint64_t timestamp =
                    host_proto::UbHostD2HInTimestamp(&interface);
                if (timestamp <= protocolTime()) {
                    serviceDevice(protocolTime());
                    continue;
                }
                break;
            }
            if (host_proto::UbHostD2HInTimestamp(&interface) > protocolTime())
                break;
            std::this_thread::yield();
            ++boundaryWaitYields;
        }
        const uint64_t incoming =
            host_proto::UbHostD2HInTimestamp(&interface);
        const uint64_t outgoing = host_proto::UbHostH2DOutNextSync(&interface);
        const uint64_t boundary = std::min(incoming, outgoing);
        if (boundary != UINT64_MAX)
            next = std::max<Tick>(curTick() + 1,
                lifecycleSyncActive ? epochOrigin + boundary : boundary);
    }
    schedule(pollEvent, next);
}

void
UbHostAdapter::handleDma(volatile host_proto::D2HMessage *message, bool read)
{
    ++dmaTransactions;
    const uint64_t request_id = message->dma.request_id;
    const uint32_t length = message->dma.length;
    const uint64_t address = message->dma.address;
    const bool msi = message->dma.address_kind ==
        static_cast<uint8_t>(host_proto::AddressKind::Msi);
    auto owned_operation = std::make_unique<DmaOperation>(
        *this, request_id, read, msi, length);
    auto *operation = owned_operation.get();
    dmaOperations.push_back(std::move(owned_operation));

    // In atomic memory mode DmaPort completes the transfer synchronously.
    // Do not attach an Event in that case: an event would be scheduled for a
    // later tick, but this request may have arrived while transactMmio() is
    // synchronously waiting for the device response.  The current PIO call
    // cannot return (and hence the event queue cannot advance) until that
    // response is sent, producing a circular wait.  Timing mode remains
    // asynchronous and uses the normal completion event.
    Event *const completion = sys->isAtomicMode() ? nullptr : &operation->done;
    if (!read) {
        const auto *source = reinterpret_cast<volatile uint8_t *>(message) +
                             sizeof(host_proto::D2HMessage);
        for (size_t i = 0; i < operation->bytes.size(); ++i)
            operation->bytes[i] = source[i];
        if (operation->msi)
            msiPort.dmaAction(MemCmd::WriteReq, address,
                              operation->bytes.size(), completion,
                              operation->bytes.data(), 0);
        else
            dmaWrite(address, operation->bytes.size(),
                     completion, operation->bytes.data());
    } else {
        dmaRead(address, operation->bytes.size(),
                completion, operation->bytes.data());
    }
    if (sys->isAtomicMode())
        completeDma(operation);
}

void
UbHostAdapter::resetProtocolEpoch()
{
    interface.base.in_timestamp = 0;
    interface.base.out_timestamp = 0;
    interface.base.sync = true;
    epochOrigin = curTick();
    serviceTime = 0;
    lifecycleSyncActive = true;
}

void
UbHostAdapter::toggleLifecycleSync()
{
    if (!lifecycleSync)
        return;
    if (!dmaOperations.empty() || !mmioCompletions.empty())
        fatal("%s: lifecycle sync requested with outstanding host operations\n",
              name());
    if (pollEvent.scheduled())
        deschedule(pollEvent);

    ++lifecycleGeneration;
    lifecycleCommitSeen = false;
    const bool enable = !lifecycleSyncActive;
    auto *message = host_proto::UbHostH2DOutAlloc(&interface, protocolTime());
    if (!message)
        fatal("%s: UB-HOST ring full at lifecycle fence\n", name());
    zeroVolatile(message->lifecycle);
    message->lifecycle.generation = lifecycleGeneration;
    message->lifecycle.action = static_cast<uint8_t>(
        host_proto::LifecycleAction::PrepareSync);
    message->lifecycle.enabled = enable ? 1 : 0;
    host_proto::UbHostH2DOutSend(&interface, message,
        static_cast<uint8_t>(host_proto::H2DType::Lifecycle));

    const auto wait_started = std::chrono::steady_clock::now();
    while (!lifecycleCommitSeen) {
        serviceDevice(UINT64_MAX);
        ++boundaryWaitYields;
        std::this_thread::yield();
    }
    if (lifecycleCommitEnabled != enable)
        fatal("%s: lifecycle fence target mismatch\n", name());
    if (enable) {
        resetProtocolEpoch();
        while (host_proto::UbHostH2DOutSync(&interface, 0) != 0) {
            ++syncBackpressure;
            std::this_thread::yield();
        }
        ++syncMessages;
    } else {
        const uint64_t absolute = protocolTime() + epochOrigin;
        interface.base.in_timestamp = absolute;
        interface.base.out_timestamp = absolute;
        interface.base.sync = false;
        lifecycleSyncActive = false;
        epochOrigin = 0;
    }
    const auto wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - wait_started).count();
    inform("[UB_HOST_FENCE] object=%s generation=%llu active=%d origin_tick=%llu wait_ns=%lld\n",
           name(), static_cast<unsigned long long>(lifecycleGeneration),
           enable ? 1 : 0,
           static_cast<unsigned long long>(epochOrigin),
           static_cast<long long>(wait_ns));
    scheduleNextPoll();
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
    if (interrupt_message.vector >= interrupts.size())
        return;
    ArmInterruptPin *interrupt = interrupts[interrupt_message.vector];
    if (!interrupt) return;
    const auto action = static_cast<host_proto::InterruptAction>(
        interrupt_message.action);
    if (action == host_proto::InterruptAction::Lower)
        interrupt->clear();
    else if (action == host_proto::InterruptAction::Raise)
        interrupt->raise();
    else {
        interrupt->raise();
        // Keep an edge visible across an event-queue boundary.  Raising and
        // clearing the ArmSPI in the same host call can make a device pulse
        // disappear before the CPU/GIC observes it, especially in atomic
        // mode.  The event owns and deletes itself after lowering the line.
        auto *clear = new EventFunctionWrapper(
            [interrupt] { interrupt->clear(); },
            name() + ".interruptClear", true);
        schedule(clear, curTick() + 1);
    }
}

} // namespace gem5
