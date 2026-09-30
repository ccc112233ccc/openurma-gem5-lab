// SPDX-License-Identifier: Apache-2.0
#include "ubsim/udma_model.h"
#include "protocol/ub_host/if.h"
#include "protocol/ub_host/proto.h"
#include "protocol/ub_net/if.h"
#include "protocol/ub_net/proto.h"

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
        Store(address, data);
        completion(true);
    }

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
        2U | (0x20U << 16) | (1U << 31); // PI=2, CQE, owner=1
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
    official_model.Receive(write_frame);
    assert(host.Load(write_target) == std::vector<std::uint8_t>({'r','m','a','!'}));
    const device::Frame write_ack = network.frames.back();
    assert(write_ack.operation == device::Frame::Operation::WriteAck);
    official_model.Receive(write_ack);
    assert(host.Contains(cq_iova + 128));

    constexpr std::uint64_t read_target = 0x180000;
    std::array<std::uint8_t, 64> read_wqe{};
    const std::uint32_t read_flags = 3U | (0x20U << 16) | (1U << 31);
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
    official_model.Receive(read_request);
    const device::Frame read_response = network.frames.back();
    assert(read_response.operation == device::Frame::Operation::ReadResponse);
    official_model.Receive(read_response);
    assert(host.Load(read_target) == std::vector<std::uint8_t>({'r','m','a','!'}));
    assert(host.Contains(cq_iova + 192));
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

    std::cout << "udma-model host/device/network separation test: PASS\n";
    return 0;
}
