// SPDX-License-Identifier: Apache-2.0
#include "ubsim/udma_model.h"
#include "protocol/ub_host/if.h"
#include "protocol/ub_host/proto.h"
#include "protocol/ub_net/if.h"
#include "protocol/ub_net/proto.h"
#include "protocol/ub_net/udma_wire.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace device = ubsim::device;

class MockHost final : public device::HostInterface {
  public:
    void Store(std::uint64_t address, const std::vector<std::uint8_t>& bytes)
    {
        memory_[address] = bytes;
    }

    template <typename T>
    void StoreObject(std::uint64_t address, const T& object)
    {
        const auto* first = reinterpret_cast<const std::uint8_t*>(&object);
        Store(address, std::vector<std::uint8_t>(first, first + sizeof(object)));
    }

    void DmaRead(std::uint64_t address, std::size_t length,
                 device::ReadCompletion completion) override
    {
        events.emplace_back("dma-read");
        if (defer_reads) {
            pending_reads_.push_back(
                PendingRead{address, length, std::move(completion)});
            return;
        }
        CompleteRead(address, length, std::move(completion));
    }

    void FlushReads()
    {
        while (!pending_reads_.empty()) {
            PendingRead pending = std::move(pending_reads_.front());
            pending_reads_.pop_front();
            CompleteRead(pending.address, pending.length,
                         std::move(pending.completion));
        }
    }

    bool defer_reads{};

  private:
    struct PendingRead {
        std::uint64_t address;
        std::size_t length;
        device::ReadCompletion completion;
    };

    void CompleteRead(std::uint64_t address, std::size_t length,
                      device::ReadCompletion completion)
    {
        const auto found = memory_.find(address);
        if (found == memory_.end() || found->second.size() != length) {
            completion(false, {});
            return;
        }
        completion(true, found->second);
    }

  public:

    void DmaWrite(std::uint64_t address, std::vector<std::uint8_t> data,
                  device::Completion completion) override
    {
        events.emplace_back("dma-write");
        dma_write_addresses.push_back(address);
        if (defer_write_address && address == defer_write_address) {
            pending_writes.push_back(
                [this, address, data = std::move(data), completion = std::move(completion)]() mutable {
                    Store(address, data);
                    completion(true);
                });
            return;
        }
        Store(address, data);
        completion(true);
    }

    void FlushWrites()
    {
        while (!pending_writes.empty()) {
            auto done = std::move(pending_writes.front());
            pending_writes.pop_front();
            done();
        }
    }

    std::uint64_t defer_write_address{};
    std::deque<std::function<void()>> pending_writes;

    void SetInterrupt(std::uint32_t vector, bool asserted) override
    {
        events.emplace_back("irq");
        irq_vector = vector;
        irq_asserted = asserted;
    }

    void PulseInterrupt(std::uint32_t vector) override
    {
        events.emplace_back("irq-pulse");
        irq_vector = vector;
        ++irq_pulses;
    }

    template <typename T>
    T LoadObject(std::uint64_t address) const
    {
        const auto& bytes = memory_.at(address);
        assert(bytes.size() == sizeof(T));
        T result{};
        std::memcpy(&result, bytes.data(), sizeof(result));
        return result;
    }

    const std::vector<std::uint8_t>& Load(std::uint64_t address) const
    {
        return memory_.at(address);
    }

    bool Contains(std::uint64_t address) const
    {
        return memory_.find(address) != memory_.end();
    }

    std::vector<std::string> events;
    std::vector<std::uint64_t> dma_write_addresses;
    std::uint32_t irq_vector{};
    bool irq_asserted{};
    std::uint32_t irq_pulses{};

  private:
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> memory_;
    std::deque<PendingRead> pending_reads_;
};

class MockNetwork final : public device::NetworkInterface {
  public:
    explicit MockNetwork(MockHost& host) : host_(host) {}

    void Send(device::Frame frame, device::Completion completion) override
    {
        host_.events.emplace_back("frame");
        frames.push_back(std::move(frame));
        completion(true);
    }

    std::vector<device::Frame> frames;

  private:
    MockHost& host_;
};

int main()
{
    static_assert(sizeof(ubsim::proto::host::H2DMessage) == 64);
    static_assert(sizeof(ubsim::proto::host::D2HMessage) == 64);
    static_assert(sizeof(ubsim::proto::net::Message) == 64);
    static_assert(ubsim::proto::net::UdmaWireOrder(
                      ubsim::proto::net::UdmaWireOrder(std::uint8_t{2})) == 2);
    static_assert(sizeof(ubsim::proto::host::Interface) ==
                  sizeof(SimbricksBaseIf));
    static_assert(sizeof(ubsim::proto::net::Interface) ==
                  sizeof(SimbricksBaseIf));

    constexpr std::uint64_t descriptor_address = 0x10000;
    constexpr std::uint64_t payload_address = 0x20000;
    constexpr std::uint64_t completion_address = 0x30000;
    const std::vector<std::uint8_t> payload{'u', 'b', '-', 'f', 'r', 'a', 'm', 'e'};

    MockHost host;
    MockNetwork network(host);
    device::UdmaModel::Config config{};
    config.extraction_test_abi = true;
    device::UdmaModel model(host, network, config);
    const device::Descriptor descriptor{
        1, 1, 0, static_cast<std::uint32_t>(payload.size()),
        0x101, 0x202, payload_address, completion_address};
    host.StoreObject(descriptor_address, descriptor);
    host.Store(payload_address, payload);

    assert(model.ReadMmio(device::UdmaModel::kRegisterIdentity, 8) ==
           device::UdmaModel::kIdentity);
    assert(model.WriteMmio(device::UdmaModel::kRegisterDoorbell, 8,
                           descriptor_address));
    assert(model.submitted() == 1);
    assert(model.completed() == 1);
    assert(network.frames.size() == 1);
    assert(network.frames[0].source_eid == 0x101);
    assert(network.frames[0].destination_eid == 0x202);
    assert(network.frames[0].source_port == 1);
    assert(network.frames[0].bytes == payload);

    const auto cqe = host.LoadObject<device::CompletionEntry>(completion_address);
    assert(cqe.sequence == 1);
    assert(cqe.status == 0);
    assert(cqe.bytes == payload.size());
    assert(host.irq_asserted && host.irq_vector == 0);
    const std::vector<std::string> expected{
        "dma-read", "dma-read", "frame", "dma-write", "irq"};
    assert(host.events == expected);

    std::stringstream checkpoint(
        std::ios::in | std::ios::out | std::ios::binary);
    assert(model.IsQuiescent());
    assert(model.SaveState(checkpoint));
    MockHost restored_host;
    MockNetwork restored_network(restored_host);
    device::UdmaModel restored_model(restored_host, restored_network, config);
    checkpoint.seekg(0);
    assert(restored_model.LoadState(checkpoint));
    assert(restored_model.submitted() == model.submitted());
    assert(restored_model.completed() == model.completed());

    // Production-mode control-plane contract copied from the official-driver
    // path: firmware discovery publishes absolute resources, UMMU capability
    // and handshake registers behave architecturally, and queue registers
    // retain driver programming.
    device::UdmaModel::Config official_config{};
    official_config.mmio_base = 0x2d000000;
    official_config.port_count = 2;
    official_config.identity_iova_test_mode = true;
    device::UdmaModel official_model(host, network, official_config);
    std::uint64_t value{};
    const std::string ubios("ubios");
    for (std::size_t i = 0; i < ubios.size(); ++i) {
        assert(official_model.ReadMmio(0x10000 + i, 1, value));
        assert(value == static_cast<std::uint8_t>(ubios[i]));
    }
    assert(official_model.ReadMmio(0x10010, 4, value) && value == 56);
    assert(official_model.ReadMmio(0x10028, 8, value) &&
           value == official_config.mmio_base + 0x11c00);
    assert(official_model.ReadMmio(0x10030, 8, value) &&
           value == official_config.mmio_base + 0x11000);
    assert(official_model.ReadMmio(0x11000 + 56 + 32, 8, value) &&
           value == official_config.mmio_base + 0x12000);
    assert(official_model.ReadMmio(0xf00010, 4, value) && value == 0x00000b08);
    assert(official_model.ReadMmio(0xf00018, 4, value) && value == 0x0000906e);
    assert(official_model.ReadMmio(0xf0001c, 4, value) && value == 0x00001010);
    assert(official_model.WriteMmio(0xf00030, 4, 0x55aa));
    assert(official_model.ReadMmio(0xf00034, 4, value) && value == 0x55aa);
    assert(official_model.WriteMmio(0xf00050, 4, 0x80000003));
    assert(official_model.ReadMmio(0xf00050, 4, value) && value == 3);
    assert(official_model.WriteMmio(0xf0117c, 4, 7));
    assert(official_model.WriteMmio(0xf01178, 4, 1));
    assert(official_model.ReadMmio(0xf0117c, 4, value) && value == 7);
    assert(official_model.ReadMmio(0xf01178, 4, value) && value == 0);
    assert(official_model.WriteMmio(0xf00108, 4, 0x80000007));
    assert(official_model.ReadMmio(0xf0010c, 4, value) &&
           value == 0x80000007);
    assert(official_model.WriteMmio(0x12010, 4, 64));
    assert(official_model.ReadMmio(0x12010, 4, value) && value == 64);
    assert(official_model.WriteMmio(0x318408, 4, 8));
    assert(official_model.ReadMmio(0x318408, 4, value) && value == 8);

    constexpr std::uint64_t ubios_sq = 0x40000;
    constexpr std::uint64_t ubios_rq = 0x50000;
    constexpr std::uint64_t ubios_cq = 0x60000;
    std::vector<std::uint8_t> ubios_sqe(16, 0);
    const auto store32 = [](std::vector<std::uint8_t>& bytes,
                            std::size_t offset, std::uint32_t word) {
        for (std::size_t i = 0; i < 4; ++i)
            bytes[offset + i] = static_cast<std::uint8_t>(word >> (8 * i));
    };
    store32(ubios_sqe, 0, (36U << 16) | (0x10U << 8));
    store32(ubios_sqe, 4, 7);
    store32(ubios_sqe, 8, 0x100);
    std::vector<std::uint8_t> token_request(36, 0);
    token_request[31] = 0x10;
    host.Store(ubios_sq, ubios_sqe);
    host.Store(ubios_sq + 0x100, token_request);
    assert(official_model.WriteMmio(0x12000, 4, ubios_sq));
    assert(official_model.WriteMmio(0x12004, 4, ubios_sq >> 32));
    assert(official_model.WriteMmio(0x12010, 4, 8));
    assert(official_model.WriteMmio(0x12040, 4, ubios_rq));
    assert(official_model.WriteMmio(0x12044, 4, ubios_rq >> 32));
    assert(official_model.WriteMmio(0x12070, 4, ubios_cq));
    assert(official_model.WriteMmio(0x12074, 4, ubios_cq >> 32));
    assert(official_model.WriteMmio(0x12008, 4, 1));
    assert(official_model.ReadMmio(0x1200c, 4, value) && value == 1);
    assert(official_model.ReadMmio(0x12048, 4, value) && value == 1);
    assert(official_model.ReadMmio(0x12078, 4, value) && value == 1);
    const auto& token_response = host.Load(ubios_rq);
    assert(token_response.size() == 40);
    assert(token_response[31] == 0x11);
    assert(token_response[32] == 1);
    assert(token_response[36] == 'M' && token_response[37] == 'R' &&
           token_response[38] == 'U' && token_response[39] == 'O');
    const auto ubios_cqe = host.LoadObject<std::array<std::uint8_t, 16>>(ubios_cq);
    assert(ubios_cqe[0] == 0 && ubios_cqe[1] == 0x11);
    assert(ubios_cqe[2] == 40 && ubios_cqe[4] == 7);

    constexpr std::uint64_t ubase_csq = 0x70000;
    std::vector<std::uint8_t> port_query(32, 0);
    port_query[0] = 0x00;
    port_query[1] = 0x62;
    port_query[3] = 1;
    host.Store(ubase_csq, port_query);
    assert(official_model.WriteMmio(0x318400, 4, ubase_csq));
    assert(official_model.WriteMmio(0x318404, 4, ubase_csq >> 32));
    assert(official_model.WriteMmio(0x318408, 4, 1));
    assert(official_model.WriteMmio(0x318410, 4, 1));
    assert(official_model.ReadMmio(0x318414, 4, value) && value == 1);
    const auto& completed_query = host.Load(ubase_csq);
    assert(completed_query.size() == 32);
    assert((completed_query[2] & 2) != 0);
    std::uint32_t port_rate{};
    std::memcpy(&port_rate, completed_query.data() + 8, sizeof(port_rate));
    assert(port_rate == 400000);
    assert(completed_query[22] == 1);

    // The unchanged UBASE driver creates event/completion/work queues through
    // opcode 0x7000.  Hardware fetches the context by IOVA, commits the CSQ
    // descriptor first, then publishes a mailbox AEQE and raises vector 1.
    constexpr std::uint64_t aeq_context_iova = 0x80000;
    constexpr std::uint64_t aeq_iova = 0x90000;
    std::vector<std::uint8_t> aeq_context(64, 0);
    store32(aeq_context, 24, 1);
    store32(aeq_context, 8, static_cast<std::uint32_t>(aeq_iova));
    host.Store(aeq_context_iova, aeq_context);
    std::vector<std::uint8_t> create_aeq(32, 0);
    create_aeq[0] = 0x00;
    create_aeq[1] = 0x70;
    create_aeq[3] = 1;
    store32(create_aeq, 8, aeq_context_iova);
    store32(create_aeq, 16, 0x34);
    store32(create_aeq, 20, (1U << 16) | 0x1234);
    host.Store(ubase_csq + 32, create_aeq);
    assert(official_model.WriteMmio(0x318410, 4, 2));
    assert(official_model.ReadMmio(0x318414, 4, value) && value == 2);
    const auto& mailbox_event = host.Load(aeq_iova);
    assert(mailbox_event.size() == 64);
    assert(mailbox_event[0] == 0x13 && (mailbox_event[3] & 0x80));
    assert(mailbox_event[12] == 0x34 && mailbox_event[13] == 0x12);
    assert(host.irq_vector == 1 && host.irq_pulses == 1);

    constexpr std::uint64_t jfc_context_iova = 0xa0000;
    constexpr std::uint64_t cq_iova = 0xb0000;
    constexpr std::uint64_t ci_iova = 0xc0000;
    std::vector<std::uint8_t> jfc_context(128, 0);
    store32(jfc_context, 0, static_cast<std::uint32_t>(cq_iova));
    store32(jfc_context, 2 * 4, 9);
    store32(jfc_context, 4 * 4, 2U << 22); // interrupt every two CQEs
    store32(jfc_context, 5 * 4, 1U << 22); // or after 4 virtual us
    store32(jfc_context, 6 * 4, static_cast<std::uint32_t>(ci_iova >> 6));
    host.Store(jfc_context_iova, jfc_context);
    std::vector<std::uint8_t> create_jfc(32, 0);
    create_jfc[0] = 0x00;
    create_jfc[1] = 0x70;
    create_jfc[3] = 1;
    store32(create_jfc, 8, jfc_context_iova);
    store32(create_jfc, 16, (7U << 8) | 0x24);
    host.Store(ubase_csq + 64, create_jfc);
    assert(official_model.WriteMmio(0x318410, 4, 3));
    assert(official_model.ReadMmio(0x318414, 4, value) && value == 3);
    assert(official_model.jfc_count() == 1);

    constexpr std::uint64_t crq_iova = 0xd0000;
    assert(official_model.WriteMmio(0x318418, 4, crq_iova));
    assert(official_model.WriteMmio(0x31841c, 4, crq_iova >> 32));
    assert(official_model.WriteMmio(0x318420, 4, 1));
    std::vector<std::uint8_t> ctrlq_request(56, 0);
    ctrlq_request[4] = 3;
    ctrlq_request[12] = 20;
    ctrlq_request[16 + 1] = 4; // QoS service
    ctrlq_request[16 + 3] = 2; // QUERY_SL
    std::vector<std::uint8_t> ctrlq_first(32, 0);
    ctrlq_first[0] = 0x0e;
    ctrlq_first[1] = 0xf0;
    ctrlq_first[3] = 2;
    std::memcpy(ctrlq_first.data() + 8, ctrlq_request.data(), 24);
    std::vector<std::uint8_t> ctrlq_continuation(32, 0);
    std::memcpy(ctrlq_continuation.data(), ctrlq_request.data() + 24, 32);
    host.Store(ubase_csq + 96, ctrlq_first);
    host.Store(ubase_csq + 128, ctrlq_continuation);
    assert(official_model.WriteMmio(0x318410, 4, 5));
    assert(official_model.ReadMmio(0x318414, 4, value) && value == 5);
    assert(official_model.ReadMmio(0x318424, 4, value) && value == 2);
    assert(official_model.ReadMmio(0x318004, 4, value) && (value & 2));
    const auto& ctrlq_response_first = host.Load(crq_iova);
    const auto& ctrlq_response_second = host.Load(crq_iova + 32);
    assert(ctrlq_response_first[0] == 0x0e && ctrlq_response_first[1] == 0xf0);
    assert(ctrlq_response_first[3] == 2);
    assert(ctrlq_response_second[4] == 1); // unic_sl_bitmap
    assert(ctrlq_response_second[8] == 1); // UDMA TP SL bitmap
    assert(ctrlq_response_second[10] == 1); // UDMA CTP SL bitmap
    assert(host.irq_vector == 0 && host.irq_pulses == 2);

    std::vector<std::uint8_t> get_tp_request(88, 0);
    get_tp_request[4] = 3;
    get_tp_request[10] = 36;
    get_tp_request[12] = 44;
    get_tp_request[16 + 1] = 1;
    get_tp_request[16 + 3] = 0x21;
    store32(get_tp_request, 28, 0x100);
    store32(get_tp_request, 44, 0x200);
    std::vector<std::uint8_t> get_tp_first(32, 0);
    get_tp_first[0] = 0x0e;
    get_tp_first[1] = 0xf0;
    get_tp_first[3] = 3;
    std::memcpy(get_tp_first.data() + 8, get_tp_request.data(), 24);
    std::vector<std::uint8_t> get_tp_second(32, 0);
    std::vector<std::uint8_t> get_tp_third(32, 0);
    std::memcpy(get_tp_second.data(), get_tp_request.data() + 24, 32);
    std::memcpy(get_tp_third.data(), get_tp_request.data() + 56, 32);
    host.Store(ubase_csq + 160, get_tp_first);
    host.Store(ubase_csq + 192, get_tp_second);
    host.Store(ubase_csq + 224, get_tp_third);
    assert(official_model.WriteMmio(0x318410, 4, 0));
    assert(official_model.tp_count() == 1);
    const auto& get_tp_response_second = host.Load(crq_iova + 3 * 32);
    std::uint32_t tp_count{};
    std::uint32_t tp_id_and_count{};
    std::memcpy(&tp_count, get_tp_response_second.data() + 4, 4);
    std::memcpy(&tp_id_and_count, get_tp_response_second.data() + 8, 4);
    assert(tp_count == 1);
    const std::uint32_t tp_id = tp_id_and_count & 0xffffffU;
    assert(tp_id != 0 && !official_model.tp_active(tp_id));
    assert(official_model.tp_port(tp_id) == 0);

    std::vector<std::uint8_t> activate_request(56, 0);
    activate_request[4] = 3;
    activate_request[10] = 24;
    activate_request[12] = 8;
    activate_request[16 + 1] = 1;
    activate_request[16 + 3] = 0x22;
    store32(activate_request, 28, tp_id | (1U << 24));
    store32(activate_request, 32, tp_id);
    std::vector<std::uint8_t> activate_first(32, 0);
    activate_first[0] = 0x0e;
    activate_first[1] = 0xf0;
    activate_first[3] = 2;
    std::memcpy(activate_first.data() + 8, activate_request.data(), 24);
    std::vector<std::uint8_t> activate_second(32, 0);
    std::memcpy(activate_second.data(), activate_request.data() + 24, 32);
    host.Store(ubase_csq, activate_first);
    host.Store(ubase_csq + 32, activate_second);
    assert(official_model.WriteMmio(0x318410, 4, 2));
    assert(official_model.tp_active(tp_id));

    constexpr std::uint64_t ceq_context_iova = 0xe0000;
    constexpr std::uint64_t ceq_iova = 0xf0000;
    std::vector<std::uint8_t> ceq_context(64, 0);
    store32(ceq_context, 8, static_cast<std::uint32_t>(ceq_iova));
    store32(ceq_context, 24, 2); // official EQ irq_num field
    host.Store(ceq_context_iova, ceq_context);
    std::vector<std::uint8_t> create_ceq(32, 0);
    create_ceq[0] = 0x00; create_ceq[1] = 0x70; create_ceq[3] = 1;
    store32(create_ceq, 8, ceq_context_iova);
    store32(create_ceq, 16, 0x44);
    host.Store(ubase_csq + 64, create_ceq);
    assert(official_model.WriteMmio(0x318410, 4, 3));

    constexpr std::uint64_t jfr_context_iova = 0x100000;
    constexpr std::uint64_t rq_iova = 0x120000;
    constexpr std::uint64_t rq_index_iova = 0x130000;
    constexpr std::uint64_t rq_producer_iova = 0x140000;
    std::vector<std::uint8_t> jfr_context(128, 0);
    store32(jfr_context, 4, static_cast<std::uint32_t>(rq_iova));
    store32(jfr_context, 8 * 4, static_cast<std::uint32_t>(rq_index_iova >> 12));
    store32(jfr_context, 9 * 4, 7U << 20);
    store32(jfr_context, 10 * 4,
            static_cast<std::uint32_t>((rq_producer_iova >> 6) << 8));
    host.Store(jfr_context_iova, jfr_context);
    std::vector<std::uint8_t> create_jfr(32, 0);
    create_jfr[0] = 0x00; create_jfr[1] = 0x70; create_jfr[3] = 1;
    store32(create_jfr, 8, jfr_context_iova);
    store32(create_jfr, 16, (11U << 8) | 0x54);
    host.Store(ubase_csq + 96, create_jfr);
    assert(official_model.WriteMmio(0x318410, 4, 4));
    assert(official_model.jfr_count() == 1);

    constexpr std::uint64_t jetty_context_iova = 0x101000;
    constexpr std::uint64_t sq_iova = 0x110000;
    std::vector<std::uint8_t> jetty_context(128, 0);
    store32(jetty_context, 0, (1U << 19) | (6U << 8)); // JETTY, 64 WQEBBs
    store32(jetty_context, 4, static_cast<std::uint32_t>(sq_iova));
    store32(jetty_context, 4 * 4, 7 | (11U << 20)); // send JFC + JFR
    store32(jetty_context, 5 * 4, 7U << 12); // receive JFC
    store32(jetty_context, 7 * 4, 0x1234); // user queue
    host.Store(jetty_context_iova, jetty_context);
    std::vector<std::uint8_t> create_jetty(32, 0);
    create_jetty[0] = 0x00; create_jetty[1] = 0x70; create_jetty[3] = 1;
    store32(create_jetty, 8, jetty_context_iova);
    store32(create_jetty, 16, (9U << 8) | 0x04);
    host.Store(ubase_csq + 128, create_jetty);
    assert(official_model.WriteMmio(0x318410, 4, 5));
    assert(official_model.jetty_count() == 1);

    // A minimal official UMMU TECT/TCT plus ARM64 L0/L1 table maps the low
    // 1 GiB IOVA range identity for the queue/payload tokens used below.
    constexpr std::uint64_t tect_iova = 0x02000000;
    constexpr std::uint64_t tct_iova = 0x02010000;
    constexpr std::uint64_t l0_iova = 0x02020000;
    constexpr std::uint64_t l1_iova = 0x02030000;
    constexpr std::uint64_t mapt_iova = 0x02040000;
    const auto store64 = [](std::vector<std::uint8_t>& bytes,
                            std::size_t offset, std::uint64_t word) {
        for (std::size_t i = 0; i < 8; ++i)
            bytes[offset + i] = static_cast<std::uint8_t>(word >> (8 * i));
    };
    std::vector<std::uint8_t> tect(64, 0);
    store64(tect, 0, 1);
    store64(tect, 8, tct_iova);
    host.Store(tect_iova, tect);
    for (const std::uint32_t token : {0U, 9U}) {
        std::vector<std::uint8_t> tct(64, 0);
        // Official TCT: valid + MAPT table mode + MAPT enabled.
        store64(tct, 0, 1 | (1U << 16) | (1U << 19));
        store64(tct, 16, l0_iova);
        store64(tct, 24, mapt_iova);
        host.Store(tct_iova + std::uint64_t(token) * 64, tct);
    }
    // The root points to a second level block in the same 64 KiB MAPT block;
    // its leaf grants RW over the low 1 GiB. Layout and bit positions are the
    // official OLK perm_table.h format.
    std::vector<std::uint8_t> mapt_root(32, 0);
    store32(mapt_root, 0, 1U | (512U << 12));
    host.Store(mapt_iova, mapt_root);
    std::vector<std::uint8_t> mapt_leaf(32, 0);
    store32(mapt_leaf, 0, 1U | (1U << 1) | (3U << 4));
    store32(mapt_leaf, 16, 0x3fffffffU);
    host.Store(mapt_iova + 512U * 32U, mapt_leaf);
    std::vector<std::uint8_t> l0_entry(8, 0);
    store64(l0_entry, 0, l1_iova | 3U);
    host.Store(l0_iova, l0_entry);
    std::vector<std::uint8_t> l1_entry(8, 0);
    store64(l1_entry, 0, 1); // identity-mapped 1 GiB L1 block
    host.Store(l1_iova, l1_entry);
    assert(official_model.WriteMmio(0xf00070, 8, tect_iova));

    // Program the Type-1 MSI tuple through the same UBIOS endpoint-config
    // messages issued by the stock UBUS MSI code.
    const auto submit_ubios = [&](std::uint32_t index, std::uint8_t task,
                                  std::uint8_t opcode,
                                  std::vector<std::uint8_t> payload) {
        const std::uint32_t payload_offset = 0x400 + index * 0x100;
        std::vector<std::uint8_t> sqe(16, 0);
        store32(sqe, 0, (static_cast<std::uint32_t>(payload.size()) << 16) |
                        (std::uint32_t(opcode) << 8) | task);
        store32(sqe, 4, index + 10);
        store32(sqe, 8, payload_offset);
        host.Store(ubios_sq + std::uint64_t(index) * 16, sqe);
        host.Store(ubios_sq + payload_offset, payload);
        assert(official_model.WriteMmio(0x12008, 4, (index + 1) % 8));
    };
    std::vector<std::uint8_t> bind_endpoint(56, 0);
    store32(bind_endpoint, 16, 1U << 8); // one endpoint hop
    bind_endpoint[25] = 2;
    store32(bind_endpoint, 52, 0x55);
    submit_ubios(1, 1, 1, std::move(bind_endpoint));
    const auto config_write = [&](std::uint32_t index, std::uint32_t reg,
                                  std::uint32_t reg_value) {
        std::vector<std::uint8_t> request(48, 0);
        store32(request, 4, 0x55);
        request[31] = 0x10;
        store32(request, 32, 0xf0);
        store32(request, 36, reg / 4);
        store32(request, 44, reg_value);
        submit_ubios(index, 0, 0x10, std::move(request));
    };
    constexpr std::uint64_t msi_address = 0x1a0000;
    config_write(2, 0x40c04, 1);
    config_write(3, 0x40c0c, 2);
    config_write(4, 0x40c10, 0x40);
    config_write(5, 0x40c14, static_cast<std::uint32_t>(msi_address));
    config_write(6, 0x40c18, 0);
    config_write(7, 0x40c20, 0);
    host.Store(ci_iova, std::vector<std::uint8_t>(4, 0));

    std::array<std::uint8_t, 64> send_wqe{};
    const std::uint32_t send_flags = 1U | (0x60U << 16) | (1U << 31);
    std::memcpy(send_wqe.data(), &send_flags, 4);
    const std::uint32_t send_command = 3U << 22;
    std::memcpy(send_wqe.data() + 4, &send_command, 4);
    std::memcpy(send_wqe.data() + 8, &tp_id, 4);
    const std::uint32_t remote_jetty = 9;
    std::memcpy(send_wqe.data() + 12, &remote_jetty, 4);
    send_wqe[48] = 'u'; send_wqe[49] = 'b'; send_wqe[50] = '!';
    constexpr std::uint64_t jetty_page = 0x00200000 + 0x1000 + 9 * 0x1000;
    for (std::size_t offset = 0; offset < send_wqe.size(); offset += 8) {
        std::uint64_t word{};
        std::memcpy(&word, send_wqe.data() + offset, 8);
        assert(official_model.WriteMmio(jetty_page + offset, 8, word));
    }
    assert(network.frames.back().operation == device::Frame::Operation::Send);
    assert(network.frames.back().source_eid == 0x100);
    assert(network.frames.back().destination_eid == 0x200);
    assert(network.frames.back().source_port == 0);
    assert(network.frames.back().destination_jetty == remote_jetty);
    assert(network.frames.back().bytes == std::vector<std::uint8_t>({'u','b','!'}));
    const auto& send_cqe = host.Load(cq_iova);
    assert(send_cqe.size() == 64);
    assert((send_cqe[0] & 6) == 6); // Jetty + owner
    assert(send_cqe[16] == 3);
    assert(!host.Contains(ceq_iova));
    assert(official_model.NextEventTime() == 4000000);
    official_model.AdvanceTime(3999999);
    assert(official_model.NextEventTime() == 4000000);
    assert(!host.Contains(ceq_iova));
    official_model.AdvanceTime(4000000);
    assert(official_model.NextEventTime() ==
           std::numeric_limits<std::uint64_t>::max());
    const auto& completion_event = host.Load(ceq_iova);
    assert((completion_event[0] & 0x7f) == 7);
    assert(host.irq_pulses == 4);
    assert(host.Contains(msi_address));
    const auto& send_msi = host.Load(msi_address);
    assert(send_msi.size() == 4 && send_msi[0] == 0x42);

    constexpr std::uint64_t receive_buffer = 0x150000;
    std::vector<std::uint8_t> posted_index(4, 0);
    std::vector<std::uint8_t> posted_producer(4, 0);
    posted_producer[0] = 1;
    std::vector<std::uint8_t> posted_sge(16, 0);
    store32(posted_sge, 0, 16);
    std::uint64_t receive_address = receive_buffer;
    std::memcpy(posted_sge.data() + 8, &receive_address, 8);
    host.Store(rq_index_iova, posted_index);
    host.Store(rq_producer_iova, posted_producer);
    host.Store(rq_iova, posted_sge);
    official_model.Receive(network.frames.back());
    assert(host.Contains(receive_buffer));
    assert(host.Load(receive_buffer) == std::vector<std::uint8_t>({'u','b','!'}));
    assert(host.Contains(cq_iova + 64));
    const auto& receive_cqe = host.Load(cq_iova + 64);
    assert((receive_cqe[0] & 7) == 7); // receive + Jetty + owner
    assert(receive_cqe[16] == 3);
    assert(host.irq_pulses == 4 && host.Load(msi_address)[0] == 0x42);

    constexpr std::uint64_t write_source = 0x160000;
    constexpr std::uint64_t write_target = 0x170000;
    host.Store(write_source, std::vector<std::uint8_t>({'r','m','a','!'}));
    std::array<std::uint8_t, 64> write_wqe{};
    const std::uint32_t write_flags =
        2U | (0x21U << 16) | (1U << 31); // PI=2, RO + CQE, owner=1
    std::memcpy(write_wqe.data(), &write_flags, 4);
    const std::uint32_t write_command = 3U << 8;
    std::memcpy(write_wqe.data() + 4, &write_command, 4);
    const std::uint32_t write_tpn_sge = tp_id | (1U << 24);
    std::memcpy(write_wqe.data() + 8, &write_tpn_sge, 4);
    std::memcpy(write_wqe.data() + 12, &remote_jetty, 4);
    std::memcpy(write_wqe.data() + 40, &write_target, 8);
    const std::uint32_t rma_bytes = 4;
    const std::uint32_t local_token = 5;
    std::memcpy(write_wqe.data() + 48, &rma_bytes, 4);
    std::memcpy(write_wqe.data() + 52, &local_token, 4);
    std::memcpy(write_wqe.data() + 56, &write_source, 8);
    for (std::size_t offset = 0; offset < write_wqe.size(); offset += 8) {
        std::uint64_t word{};
        std::memcpy(&word, write_wqe.data() + offset, 8);
        assert(official_model.WriteMmio(jetty_page + offset, 8, word));
    }
    const device::Frame write_frame = network.frames.back();
    assert(write_frame.operation == device::Frame::Operation::Write);
    assert(official_model.rma_pending_peak() == 1);
    assert(write_frame.order_type == 1); // official SQE place_odr=RO
    official_model.Receive(write_frame);
    assert(host.Load(write_target) == std::vector<std::uint8_t>({'r','m','a','!'}));
    const device::Frame write_ack = network.frames.back();
    assert(write_ack.operation == device::Frame::Operation::WriteAck);
    official_model.Receive(write_ack);
    assert(host.Contains(cq_iova + 128));

    constexpr std::uint64_t read_target = 0x180000;
    std::array<std::uint8_t, 64> read_wqe{};
    const std::uint32_t read_flags =
        3U | (0x22U << 16) | (1U << 31); // PI=3, SO + CQE, owner=1
    std::memcpy(read_wqe.data(), &read_flags, 4);
    const std::uint32_t read_command = 6U << 8;
    std::memcpy(read_wqe.data() + 4, &read_command, 4);
    std::memcpy(read_wqe.data() + 8, &write_tpn_sge, 4);
    std::memcpy(read_wqe.data() + 12, &remote_jetty, 4);
    std::memcpy(read_wqe.data() + 40, &write_target, 8);
    std::memcpy(read_wqe.data() + 48, &rma_bytes, 4);
    std::memcpy(read_wqe.data() + 52, &local_token, 4);
    std::memcpy(read_wqe.data() + 56, &read_target, 8);
    for (std::size_t offset = 0; offset < read_wqe.size(); offset += 8) {
        std::uint64_t word{};
        std::memcpy(&word, read_wqe.data() + offset, 8);
        assert(official_model.WriteMmio(jetty_page + offset, 8, word));
    }
    const device::Frame read_request = network.frames.back();
    assert(read_request.operation == device::Frame::Operation::ReadRequest);
    assert(read_request.order_type == 2); // official SQE place_odr=SO
    official_model.Receive(read_request);
    const device::Frame read_response = network.frames.back();
    assert(read_response.operation == device::Frame::Operation::ReadResponse);
    official_model.Receive(read_response);
    assert(host.Load(read_target) == std::vector<std::uint8_t>({'r','m','a','!'}));
    assert(host.Contains(cq_iova + 192));
    assert(official_model.rma_pending_peak() == 1);
    assert(official_model.sq_wqebbs_retired() == 3);
    assert(host.irq_pulses == 4 && host.Load(msi_address)[0] == 0x42);

    // A busy device can observe several monotonically increasing doorbells
    // before the first SQ DMA completes.  An old callback must not overwrite
    // the newest target producer (the real send_bw/post-list failure mode).
    std::array<std::uint8_t, 64> queued_wqe = send_wqe;
    const std::uint32_t queued_flags =
        3U | (0x40U << 16) | (1U << 31); // PI=3, inline, no CQE, owner=1
    std::memcpy(queued_wqe.data(), &queued_flags, 4);
    host.Store(sq_iova + 3 * 64,
               std::vector<std::uint8_t>(queued_wqe.begin(), queued_wqe.end()));
    const std::uint32_t queued_flags_2 =
        4U | (0x40U << 16) | (1U << 31); // PI=4
    std::memcpy(queued_wqe.data(), &queued_flags_2, 4);
    host.Store(sq_iova + 4 * 64,
               std::vector<std::uint8_t>(queued_wqe.begin(), queued_wqe.end()));
    const std::size_t frames_before_coalesced = network.frames.size();
    host.defer_reads = true;
    assert(official_model.WriteMmio(jetty_page + 0x80, 4, 4));
    assert(official_model.WriteMmio(jetty_page + 0x80, 4, 5));
    host.FlushReads();
    host.defer_reads = false;
    assert(network.frames.size() == frames_before_coalesced + 2);
    assert(official_model.sq_doorbells() == 2);
    assert(official_model.sq_completions() >= 5);

    // Inline WQEs can span multiple 64-byte WQEBBs.  The RMA acknowledgement
    // must advance the SQ consumer by the decoded WQE size, not by one.  A
    // one-block advance would make the continuation payload look like the
    // next WQE and permanently stall the queue.
    std::array<std::uint8_t, 128> inline_write{};
    const std::uint32_t inline_write_flags =
        5U | (0x60U << 16) | (1U << 31); // slot 5, inline + CQE, owner=1
    std::memcpy(inline_write.data(), &inline_write_flags, 4);
    const std::uint32_t inline_write_command =
        (3U << 8) | (32U << 22); // WRITE, 32-byte inline payload
    std::memcpy(inline_write.data() + 4, &inline_write_command, 4);
    std::memcpy(inline_write.data() + 8, &tp_id, 4);
    std::memcpy(inline_write.data() + 12, &remote_jetty, 4);
    std::memcpy(inline_write.data() + 40, &write_target, 8);
    for (std::size_t i = 0; i < 32; ++i)
        inline_write[48 + i] = static_cast<std::uint8_t>(i);
    host.Store(sq_iova + 5 * 64,
               std::vector<std::uint8_t>(inline_write.begin(),
                                         inline_write.begin() + 64));
    host.Store(sq_iova + 6 * 64,
               std::vector<std::uint8_t>(inline_write.begin() + 64,
                                         inline_write.end()));
    assert(official_model.WriteMmio(jetty_page + 0x80, 4, 7));
    const device::Frame inline_write_frame = network.frames.back();
    assert(inline_write_frame.operation == device::Frame::Operation::Write);
    assert(inline_write_frame.bytes.size() == 32);
    official_model.Receive(inline_write_frame);
    const device::Frame inline_write_ack = network.frames.back();
    assert(inline_write_ack.operation == device::Frame::Operation::WriteAck);
    official_model.Receive(inline_write_ack);
    const auto& inline_write_cqe = host.Load(cq_iova + 4 * 64);
    const std::uint16_t inline_write_entry =
        std::uint16_t(inline_write_cqe[4]) |
        (std::uint16_t(inline_write_cqe[5]) << 8);
    assert(inline_write_entry == 6); // final block of the slot-5/6 WQE
    assert(official_model.sq_wqebbs_retired() == 7);

    std::array<std::uint8_t, 64> after_multi_wqebb = send_wqe;
    const std::uint32_t after_multi_flags =
        7U | (0x40U << 16) | (1U << 31); // slot 7, inline, owner=1
    std::memcpy(after_multi_wqebb.data(), &after_multi_flags, 4);
    host.Store(sq_iova + 7 * 64,
               std::vector<std::uint8_t>(after_multi_wqebb.begin(),
                                         after_multi_wqebb.end()));
    const std::size_t frames_before_after_multi = network.frames.size();
    assert(official_model.WriteMmio(jetty_page + 0x80, 4, 8));
    assert(network.frames.size() == frames_before_after_multi + 1);
    assert(network.frames.back().operation == device::Frame::Operation::Send);

    device::Frame invalid_token_write{};
    invalid_token_write.operation = device::Frame::Operation::Write;
    invalid_token_write.segment = 100;
    invalid_token_write.remote_address = 0x190000;
    invalid_token_write.transfer_length = 1;
    invalid_token_write.bytes = {'x'};
    official_model.Receive(std::move(invalid_token_write));
    assert(!host.Contains(0x190000));
    official_model.SetLinkState(0, false);
    assert(official_model.tp_port(tp_id) == 1);
    assert(!official_model.ReadMmio(
        device::UdmaModel::kOfficialApertureBytes, 1, value));

    // Exercise lifecycle commands through the same CSQ DMA/MMIO boundary as
    // the official driver, including ring wrap. Do not call model internals.
    const auto mailbox = [&](std::vector<std::uint8_t> desc, std::uint16_t expected=0) {
        std::uint64_t head{};
        assert(official_model.ReadMmio(0x318414, 4, head));
        const auto slot = head % 8;
        host.Store(ubase_csq + slot * 32, std::move(desc));
        assert(official_model.WriteMmio(0x318410, 4, (slot + 1) % 8));
        assert(official_model.ReadMmio(0x318414, 4, head));
        assert(head == (slot + 1) % 8);
        const auto& completed = host.Load(ubase_csq + slot * 32);
        assert((completed[2] & 2) && completed[4]==(expected&255) && completed[5]==(expected>>8));
        return completed;
    };
    const auto destroy = [&](std::uint8_t opcode, std::uint32_t id) {
        std::vector<std::uint8_t> desc(32, 0);
        desc[1] = 0x70; desc[3] = 1;
        store32(desc, 16, (id << 8) | opcode);
        mailbox(std::move(desc));
    };
    const auto drain_destroy = [&](std::vector<std::uint8_t> desc) {
        std::uint64_t head{}, after{};
        assert(official_model.ReadMmio(0x318414, 4, head));
        const auto errors = official_model.ubase_errors();
        desc[2] &= ~2U;
        host.Store(ubase_csq + head * 32, desc);
        std::vector<std::uint8_t> follower(32, 0);
        follower[0] = 7; follower[3] = 1; // NOTIFY_DRV_CAPS after destroy
        host.Store(ubase_csq + ((head + 1) % 8) * 32, follower);
        assert(official_model.WriteMmio(0x318410, 4, (head + 2) % 8));
        assert(official_model.ReadMmio(0x318414, 4, after) && after == head);
        assert(!(host.Load(ubase_csq + head * 32)[2] & 2));
        assert(!(host.Load(ubase_csq + ((head + 1) % 8) * 32)[2] & 2));
        assert(!official_model.IsQuiescent());
        // Finish the event write, but hold its Type-1 MSI completion next.
        assert(host.pending_writes.size() == 1);
        host.defer_write_address = msi_address;
        auto event_done = std::move(host.pending_writes.front());
        host.pending_writes.pop_front();
        event_done();
        assert(host.pending_writes.size() == 1);
        assert(official_model.ReadMmio(0x318414, 4, after) && after == head);
        assert(!(host.Load(ubase_csq + head * 32)[2] & 2));
        host.defer_write_address = 0;
        host.FlushWrites();
        assert(official_model.ReadMmio(0x318414, 4, after) && after == (head + 2) % 8);
        for (unsigned i = 0; i < 2; ++i) {
            const auto& done = host.Load(ubase_csq + ((head + i) % 8) * 32);
            assert((done[2] & 2) && !done[4] && !done[5]);
        }
        assert(official_model.ubase_errors() == errors);
    };
    // Hold actual event DMA completion while later CSQ commands continue.
    host.defer_write_address = aeq_iova;
    mailbox(create_aeq); // producer resets to zero; event DMA remains in flight
    assert(host.pending_writes.size() == 1);
    assert(!official_model.IsQuiescent());
    std::ostringstream busy_snapshot;
    assert(!official_model.SaveState(busy_snapshot));
    std::istringstream busy_restore("not consumed while DMA is outstanding");
    assert(!official_model.LoadState(busy_restore));
    assert(busy_restore.tellg() == 0);
    auto queued_mailbox = create_aeq;
    store32(queued_mailbox, 16, 0); // write-context command, event-enabled completion
    store32(queued_mailbox, 20, (1U << 16) | 0x1235);
    mailbox(queued_mailbox);
    assert(host.pending_writes.size() == 1); // second event is queued, not same-slot DMA
    auto destroy_aeq = create_aeq;
    store32(destroy_aeq, 16, 0x37);
    store32(destroy_aeq, 20, 0); // official EQ teardown uses polling
    mailbox(create_aeq, 16);
    assert(official_model.aeq_enabled());
    drain_destroy(destroy_aeq);
    assert(host.Load(aeq_iova)[12] == 0x34);
    assert(host.Load(aeq_iova + 64)[12] == 0x35);
    assert(!official_model.aeq_enabled());
    const auto old_aeq_writes = host.dma_write_addresses.size();
    auto recreated_aeq = create_aeq;
    constexpr std::uint64_t new_aeq_iova = 0x1b0000;
    auto new_aeq_context = aeq_context;
    store32(new_aeq_context, 8, new_aeq_iova);
    host.Store(aeq_context_iova, new_aeq_context);
    mailbox(recreated_aeq);
    assert(official_model.aeq_enabled());
    assert(host.Load(new_aeq_iova)[12] == 0x34);
    for (std::size_t i = old_aeq_writes; i < host.dma_write_addresses.size(); ++i)
        assert(host.dma_write_addresses[i] < aeq_iova ||
               host.dma_write_addresses[i] >= aeq_iova + 64 * 64);
    std::cout << "udma-model AEQ in-flight teardown/recreate and checkpoint guard: PASS\n";
    const auto errors_before_lifecycle = official_model.ubase_errors();
    assert(official_model.ceq_enabled());
    destroy(0x47, 0);
    assert(!official_model.ceq_enabled());
    mailbox(create_ceq);
    assert(official_model.ceq_enabled());
    destroy(0x47, 0);
    assert(!official_model.ceq_enabled());
    const auto jfcs_before = official_model.jfc_count();
    assert(jfcs_before > 0);
    for (unsigned round = 0; round < 16; ++round) {
        destroy(0x27, 7);
        assert(official_model.jfc_count() == jfcs_before - 1);
        mailbox(create_jfc);
        assert(official_model.jfc_count() == jfcs_before);
    }
    assert(official_model.ubase_errors() == errors_before_lifecycle);
    const auto dma_before_destroyed_ceq = host.dma_write_addresses.size();
    for (std::uint32_t index = 8; index < 10; ++index) {
        auto wqe = send_wqe;
        const auto flags = index | (0x60U << 16) | (1U << 31);
        std::memcpy(wqe.data(), &flags, 4);
        host.Store(sq_iova + index * 64, {wqe.begin(), wqe.end()});
    }
    const auto completions_before_destroyed_ceq = official_model.sq_completions();
    assert(official_model.WriteMmio(jetty_page + 0x80, 4, 10));
    // First CQE is below moderation threshold; the second needs a CEQ and
    // must fail notification rather than DMA into the destroyed buffer.
    // Both CQEs are already visible: notification failure cannot replay a WQE.
    assert(official_model.sq_completions() == completions_before_destroyed_ceq + 2);
    assert(official_model.ubase_errors() == errors_before_lifecycle + 1);
    bool saw_cqe = false;
    for (std::size_t i = dma_before_destroyed_ceq; i < host.dma_write_addresses.size(); ++i) {
        const auto address = host.dma_write_addresses[i];
        assert(address < ceq_iova || address >= ceq_iova + 4096);
        saw_cqe |= address >= cq_iova && address < cq_iova + 4096;
    }
    assert(saw_cqe); // real CQ completion occurred, but no DMA to freed CEQ memory
    mailbox(create_ceq);
    host.defer_write_address = ceq_iova;
    for (std::uint32_t index = 10; index < 12; ++index) {
        auto wqe = send_wqe;
        const auto flags = index | (0x60U << 16) | (1U << 31);
        std::memcpy(wqe.data(), &flags, 4);
        host.Store(sq_iova + index * 64, {wqe.begin(), wqe.end()});
    }
    assert(official_model.WriteMmio(jetty_page + 0x80, 4, 12));
    assert(host.pending_writes.size() == 1);
    auto destroy_ceq = create_ceq;
    store32(destroy_ceq, 16, 0x47);
    mailbox(create_ceq, 16);
    assert(official_model.ceq_enabled());
    assert(!official_model.IsQuiescent());
    drain_destroy(destroy_ceq);
    assert(!official_model.ceq_enabled());
    std::cout << "udma-model CEQ in-flight destroy/replacement guard: PASS\n";
    // Two CEQ IDs own independent rings and use the official irq_num field.
    mailbox(create_ceq);
    auto ceq1_context = ceq_context;
    constexpr std::uint64_t ceq1_iova = 0x1c0000;
    store32(ceq1_context, 8, ceq1_iova);
    store32(ceq1_context, 24, 3);
    host.Store(ceq_context_iova, ceq1_context);
    auto create_ceq1 = create_ceq;
    store32(create_ceq1, 16, (1U << 8) | 0x44);
    mailbox(create_ceq1);
    assert(official_model.ceq_enabled(0) && official_model.ceq_enabled(1));
    destroy(0x27, 7);
    auto jfc1_context = jfc_context;
    store32(jfc1_context, 8, (1U << 24) | 9); // CEQN=1, original token
    host.Store(jfc_context_iova, jfc1_context);
    mailbox(create_jfc);
    const auto submit_pair = [&](unsigned index) {
        for (unsigned i = index; i < index + 2; ++i) {
            auto wqe = send_wqe;
            const auto flags = i | (0x60U << 16) | (1U << 31);
            std::memcpy(wqe.data(), &flags, 4);
            host.Store(sq_iova + i * 64, {wqe.begin(), wqe.end()});
        }
        assert(official_model.WriteMmio(jetty_page + 0x80, 4, index + 2));
    };
    submit_pair(12);
    assert(host.Load(ceq1_iova)[0] == 7 && host.Load(msi_address)[0] == 0x43);
    destroy(0x47, 0);
    assert(!official_model.ceq_enabled(0) && official_model.ceq_enabled(1));
    submit_pair(14);
    assert(host.Load(ceq1_iova + 64)[0] == 7);
    std::stringstream multi_ceq_checkpoint(std::ios::in | std::ios::out | std::ios::binary);
    assert(official_model.SaveState(multi_ceq_checkpoint));
    device::UdmaModel restored_multi_ceq(host, network, official_config);
    assert(restored_multi_ceq.LoadState(multi_ceq_checkpoint));
    assert(!restored_multi_ceq.ceq_enabled(0) && restored_multi_ceq.ceq_enabled(1));
    std::cout << "udma-model multi-CEQ routing, independent destroy, MSI and checkpoint: PASS\n";
    // Count retired WQEBBs, not WQEs: the two-block descriptors below cross
    // a four-slot SQ boundary, and owner toggles must still be respected.
    constexpr std::uint64_t small_sq = 0x210000;
    constexpr std::uint64_t other_sq = 0x220000;
    const auto create_small_jetty = [&](std::uint32_t id, std::uint64_t address) {
        auto context = jetty_context;
        store32(context, 0, (1U << 19) | (2U << 8)); // four WQEBBs
        store32(context, 4, static_cast<std::uint32_t>(address));
        host.Store(jetty_context_iova, context);
        auto command = create_jetty;
        store32(command, 16, (id << 8) | 0x04);
        mailbox(command);
    };
    create_small_jetty(10, small_sq);
    create_small_jetty(11, other_sq);
    constexpr std::uint64_t small_page = 0x00200000 + 0x1000 + 10 * 0x1000;
    constexpr std::uint64_t other_page = 0x00200000 + 0x1000 + 11 * 0x1000;
    const auto retired_before = official_model.sq_wqebbs_retired();
    const auto wraps_before = official_model.sq_consumer_wraps();
    const auto completions_before_wrap = official_model.sq_completions();
    const auto post_small_send = [&](std::uint32_t consumer, unsigned blocks) {
        std::vector<std::uint8_t> raw(blocks * 64, 0);
        const auto owner = ((consumer / 4) & 1U) ? 0U : (1U << 31);
        store32(raw, 0, consumer | (0x40U << 16) | owner); // no CQE
        store32(raw, 4, (blocks == 2 ? 32U : 3U) << 22);
        store32(raw, 8, tp_id);
        store32(raw, 12, remote_jetty);
        for (unsigned i = 48; i < (blocks == 2 ? 80U : 51U); ++i)
            raw[i] = static_cast<std::uint8_t>(i);
        for (unsigned i = 0; i < blocks; ++i)
            host.Store(small_sq + ((consumer + i) % 4) * 64,
                {raw.begin() + i * 64, raw.begin() + (i + 1) * 64});
        assert(official_model.WriteMmio(small_page + 0x80, 4, consumer + blocks));
    };
    post_small_send(0, 1);
    post_small_send(1, 1);
    post_small_send(2, 1);
    assert(official_model.sq_consumer_wraps() == wraps_before);
    post_small_send(3, 2); // descriptor itself spans slots 3 and 0
    assert(official_model.sq_consumer_wraps() == wraps_before + 1);
    post_small_send(5, 2);
    post_small_send(7, 1); // exact boundary also counts as a wrap
    assert(official_model.sq_consumer_wraps() == wraps_before + 2);
    assert(official_model.sq_wqebbs_retired() == retired_before + 8);
    assert(official_model.sq_completions() == completions_before_wrap + 6);

    // Holding one WRITE ACK per Jetty demonstrates device-level concurrency;
    // this does not claim more than one active WQE within either Jetty.
    auto pending_write = write_wqe;
    const std::uint32_t pending_flags = 8U | (0x01U << 16) | (1U << 31);
    std::memcpy(pending_write.data(), &pending_flags, 4);
    host.Store(small_sq, {pending_write.begin(), pending_write.end()});
    assert(official_model.WriteMmio(small_page + 0x80, 4, 9));
    const auto first_pending_write = network.frames.back();
    assert(official_model.sq_wqebbs_retired() == retired_before + 8);
    host.Store(other_sq, {pending_write.begin(), pending_write.end()});
    assert(official_model.WriteMmio(other_page + 0x80, 4, 1));
    const auto second_pending_write = network.frames.back();
    assert(first_pending_write.request_id != second_pending_write.request_id);
    assert(official_model.rma_pending_peak() == 2);
    official_model.Receive(first_pending_write);
    const auto first_pending_ack = network.frames.back();
    official_model.Receive(second_pending_write);
    const auto second_pending_ack = network.frames.back();
    official_model.Receive(first_pending_ack);
    official_model.Receive(second_pending_ack);
    assert(official_model.sq_wqebbs_retired() == retired_before + 10);
    assert(official_model.sq_consumer_wraps() == wraps_before + 2);
    assert(official_model.rma_pending_peak() == 2); // high-water mark survives drain
    std::cout << "udma-model retired WQEBBs, SQ wraps and device RMA peak: PASS\n";
    auto duplicate_jfc=create_jfc;
    mailbox(duplicate_jfc,17);
    auto bad_context=create_jfc;
    store32(bad_context,16,(99U<<8)|0x24);
    store32(bad_context,8,0); store32(bad_context,12,0);
    mailbox(bad_context,22);
    store32(bad_context,8,0x1a0000); // no mapped context bytes in mock memory
    mailbox(bad_context,5);
    host.Store(0x1a0000,std::vector<std::uint8_t>(128,0));
    mailbox(bad_context,22); // mapped but invalid queue addresses
    assert(official_model.jfc_count()==jfcs_before);
    auto probe=std::vector<std::uint8_t>(32,0);
    probe[0]=0x30; probe[3]=1; probe[2]=1U<<6;
    assert(mailbox(probe)[8]==10); // 312-byte query uses ten descriptors
    probe[2]=0;
    mailbox(probe,90); // do not silently truncate a large response
    probe[0]=7; // subsequent NOTIFY_DRV_CAPS still completes
    mailbox(probe);
    probe[3]=7; // chain extends past the producer: consume only the malformed header
    mailbox(probe,22);
    probe[3]=1;
    mailbox(probe);
    probe[0]=0x13; probe[1]=0x42;
    mailbox(probe,95);
    probe[0]=0x14;
    mailbox(probe,95);
    probe[0]=7; probe[1]=0;
    mailbox(probe); // diagnostics cannot poison the following command
    // Control-path failures must complete with Linux errno and leave CSQ usable.
    const auto control = [&](std::vector<std::uint8_t> request, std::uint16_t expected) {
        std::uint64_t head{}, tail{};
        assert(official_model.ReadMmio(0x318414, 4, head));
        assert(official_model.ReadMmio(0x318424, 4, tail));
        const auto count = (request.size() + 39) / 32;
        assert(count < 8);
        std::size_t offset = 0;
        for (std::size_t i=0; i<count; ++i) {
            std::vector<std::uint8_t> desc(32, 0);
            const std::size_t start = i ? 0 : 8;
            if (!i) { desc[0]=0x0e; desc[1]=0xf0; desc[3]=count; }
            const auto bytes = std::min(32-start, request.size()-offset);
            std::memcpy(desc.data()+start, request.data()+offset, bytes);
            offset += bytes;
            host.Store(ubase_csq + ((head+i)%8)*32, desc);
        }
        const auto errors = official_model.ubase_errors();
        const auto target = (head+count)%8;
        assert(official_model.WriteMmio(0x318410, 4, target));
        if (host.defer_reads) host.FlushReads();
        const auto& completed = host.Load(ubase_csq + head*32);
        if (completed[4]!=(expected&255) || completed[5]!=(expected>>8))
            std::cerr << "CtrlQ opcode=" << unsigned(request[19]) << " expected=" << expected
                      << " actual=" << (unsigned(completed[4]) | (unsigned(completed[5])<<8)) << '\n';
        assert((completed[2]&2) && completed[4]==(expected&255) && completed[5]==(expected>>8));
        std::uint64_t after{};
        assert(official_model.ReadMmio(0x318414, 4, after) && after==target);
        assert(official_model.ubase_errors()==errors+(expected ? 1 : 0));
        assert(official_model.ReadMmio(0x318424, 4, after));
        if (expected) { assert(after==tail); return std::vector<std::uint8_t>{}; }
        const auto& first = host.Load(crq_iova+tail*32);
        std::vector<std::uint8_t> result(first.begin()+8,first.end());
        for (unsigned i=1;i<first[3];++i) {
            const auto& part = host.Load(crq_iova+((tail+i)%8)*32);
            result.insert(result.end(),part.begin(),part.end());
        }
        assert(official_model.WriteMmio(0x318428,4,after)); // consume response
        return result;
    };
    const auto request = [&](std::uint8_t op, unsigned input, unsigned output, std::uint32_t id=0) {
        std::vector<std::uint8_t> bytes(28+input,0);
        bytes[4]=3; bytes[10]=input; bytes[11]=input>>8;
        bytes[12]=output; bytes[13]=output>>8;
        bytes[17]=1; bytes[19]=op;
        if (input>=4) store32(bytes,28,id);
        return bytes;
    };
    const auto routes_before = official_model.tp_count();
    control(request(0xff,0,0),95); // unsupported is not success
    control(request(0x21,35,44),22); // short GET_LIST
    control(request(0x21,36,43),22); // short output
    auto truncated=request(0x21,36,44); truncated.resize(28);
    control(truncated,22); // declared input exceeds descriptor DMA
    control(request(0x22,23,8,tp_id),22); // full official ACTIVE is 24 bytes
    control(request(0x23,11,0,tp_id),22); // DEACTIVE is 12 bytes
    control(request(0x22,24,8,999),2); // unknown TP
    control(request(0x21,36,65535),90); // response cannot fit CRQ
    assert(official_model.tp_count()==routes_before && official_model.tp_active(tp_id));
    auto allocate=request(0x21,36,44);
    store32(allocate,28,0x100); store32(allocate,44,0x200);
    official_model.SetLinkState(1,false);
    control(allocate,100); // neither port can carry a TP
    control(request(0x22,24,8,tp_id),100);
    assert(official_model.tp_count()==routes_before);
    official_model.SetLinkState(1,true);
    control(request(0x22,24,8,tp_id),0);
    for (unsigned i=routes_before;i<1023;++i) control(allocate,0);
    assert(official_model.tp_count()==1023);
    control(allocate,28); // full table, no overwrite
    control(request(0x13,4,4,tp_id),16); // reject deleting active route
    control(request(0x23,12,0,tp_id),0);
    assert(!official_model.tp_active(tp_id));
    control(request(0x13,4,4,tp_id),0);
    assert(official_model.tp_count()==1022);
    control(request(0x13,4,4,tp_id),2); // double remove is visible
    const auto reused=control(allocate,0); // only the freed slot can be allocated
    std::uint32_t reused_id{};
    std::memcpy(&reused_id,reused.data()+32,4);
    assert((reused_id&0xffffffU)==tp_id && official_model.tp_count()==1023);
    for (unsigned id=1;id<=1023;++id) control(request(0x13,4,4,id),0);
    assert(official_model.tp_count()==0);
    control(allocate,0); // success after failures, wrap, exhaustion and reclamation
    assert(official_model.tp_count()==1);
    host.defer_reads=true;
    control(allocate,0); // descriptor + continuation DMA callbacks remain valid asynchronously
    host.defer_reads=false;
    assert(official_model.tp_count()==2);
    std::cout << "udma-model CtrlQ errors, 1023 TP exhaustion/reclaim/reuse: PASS\n";
    std::cout << "udma-model CEQ destroy/recreate and JFC lifecycle/ring-wrap: PASS\n";
    std::cout << "udma-model host/device/network separation test: PASS\n";
    return 0;
}
