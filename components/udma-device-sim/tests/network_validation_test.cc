// SPDX-License-Identifier: Apache-2.0
// Real NetworkPort parsing on in-memory rings; no socket or private macro.
#define main udma_device_program_main
#include "../src/main.cc"
#undef main
#include <array>
#include <cassert>
#include <sstream>
#include <unistd.h>

namespace {
struct TestHost final : device::HostInterface {
    void DmaRead(std::uint64_t, std::size_t, device::ReadCompletion done) override { done(false, {}); }
    void DmaWrite(std::uint64_t, std::vector<std::uint8_t>, device::Completion done) override { done(false); }
    void SetInterrupt(std::uint32_t, bool) override {}
};
struct Fixture {
    std::array<std::uint64_t, 256> incoming{}, outgoing{};
    net_proto::Interface interface{};
    std::uint64_t now{};
    NetworkPort port{interface, now};
    TestHost host;
    device::UdmaModel model{host, port};
    Fixture() {
        interface.base.in_queue = incoming.data(); interface.base.out_queue = outgoing.data();
        interface.base.in_elen = sizeof(incoming); interface.base.out_elen = sizeof(outgoing);
        interface.base.in_enum = interface.base.out_enum = 1; port.Attach(&model);
    }
    net_proto::Message& Message(net_proto::MessageType type) {
        incoming.fill(0);
        auto& message = *reinterpret_cast<net_proto::Message*>(incoming.data());
        message.base.header.own_type = static_cast<std::uint8_t>(type) | SIMBRICKS_PROTO_MSG_OWN_CON;
        return message;
    }
    void Commit(std::uint64_t generation, std::uint8_t enabled = 1) {
        auto& message = Message(net_proto::MessageType::Lifecycle);
        message.lifecycle.generation = generation; message.lifecycle.enabled = enabled;
        message.lifecycle.action = static_cast<std::uint8_t>(net_proto::LifecycleAction::CommitSync);
    }
    void Fragment(std::uint32_t offset, std::uint32_t length, bool last,
                  std::uint32_t total = 4, std::uint32_t source = 1) {
        auto& message = Message(net_proto::MessageType::Frame);
        message.frame.sequence = 9; message.frame.source_eid = source; message.frame.destination_eid = 2;
        message.frame.length = sizeof(net_proto::UdmaWireHeader) + length;
        net_proto::UdmaWireHeader header{};
        header.magic = net_proto::kUdmaWireMagic; header.version = net_proto::kUdmaWireVersion;
        header.operation = static_cast<std::uint8_t>(device::Frame::Operation::Send);
        header.flags = net_proto::kUdmaWireFragmented | (last ? net_proto::kUdmaWireLastFragment : 0);
        header.transfer_length = total; header.payload_offset = offset; header.payload_length = length;
        auto* payload = reinterpret_cast<std::uint8_t*>(incoming.data()) + sizeof(message);
        assert(sizeof(message) + sizeof(header) + length <= sizeof(incoming));
        std::memcpy(payload, &header, sizeof(header)); std::memset(payload + sizeof(header), 0x42, length);
    }
    void MustFail() {
        bool failed{};
        try { port.Poll(); } catch (const std::runtime_error&) { failed = true; }
        assert(failed);
    }
};
}
int main() {
    {
        Fixture f;
        device::Frame frame{};
        frame.source_port = 1;
        assert(f.port.SendWire(frame, {1, 2, 3}));
        assert(!f.port.SendWire(frame, {4, 5})); // occupied ring: not sent
        f.Fragment(0, 2, false);
        assert(f.port.Poll());
        f.Fragment(2, 2, true);
        assert(f.port.Poll());
        std::ostringstream counts;
        f.port.DumpPorts(counts);
        const auto rx_bytes = 2 * (sizeof(net_proto::UdmaWireHeader) + 2);
        assert(counts.str() == "[UDMA_PORT_PROFILE] port=0 tx_frames=0 tx_wire_bytes=0 rx_frames=2 rx_wire_bytes=" +
            std::to_string(rx_bytes) + "\n[UDMA_PORT_PROFILE] port=1 tx_frames=1 tx_wire_bytes=3 rx_frames=0 rx_wire_bytes=0\n");
    }
    {
        Fixture f;
        host_proto::Interface host_interface{};
        HostPort host_port{host_interface, f.now};
        char path[] = "/tmp/ubsim-checkpoint-XXXXXX";
        const int fd = mkstemp(path); assert(fd >= 0); close(fd);
        { std::ofstream output(path); output << "prior-checkpoint"; }
        assert(f.port.SendPrepare(1, true));
        assert(!SaveDeviceState(path, host_port, f.port, f.model));
        f.port.FinishFence();
        f.Fragment(0, 2, false); assert(f.port.Poll());
        assert(f.model.IsQuiescent()); // model alone cannot see adapter half-frame
        assert(!SaveDeviceState(path, host_port, f.port, f.model));
        { std::ifstream input(path); std::string saved; input >> saved;
          assert(saved == "prior-checkpoint"); }
        f.Fragment(2, 2, true); assert(f.port.Poll());
        assert(SaveDeviceState(path, host_port, f.port, f.model));
        { std::ifstream input(path, std::ios::binary | std::ios::ate);
          assert(input.tellg() > 16); }
        assert(std::remove(path) == 0);
    }
    { Fixture f; f.Commit(1); f.MustFail(); assert(!f.port.CommitSeen()); }
    {
        Fixture f; assert(f.port.SendPrepare(1, true)); f.Commit(1);
        assert(PollNetworkBatch(f.port));
        assert(f.port.CommitSeen());
        // Simulate the peer having queued its first epoch SYNC immediately
        // behind COMMIT. It must remain unconsumed until the reset is done.
        auto& sync = f.Message(static_cast<net_proto::MessageType>(SIMBRICKS_PROTO_MSG_TYPE_SYNC));
        sync.base.header.timestamp = 100000;
        assert(!PollNetworkBatch(f.port));
        assert(sync.base.header.own_type & SIMBRICKS_PROTO_MSG_OWN_CON);
        f.interface.base.in_timestamp = f.interface.base.out_timestamp = 0;
        f.interface.base.sync = true;
        f.now = 100000;
        f.port.FinishFence();
        PollNetworkBatch(f.port);
        assert(f.interface.base.in_timestamp == 100000);
        assert(!(sync.base.header.own_type & SIMBRICKS_PROTO_MSG_OWN_CON));
    }
    for (const auto generation : {0ULL, 2ULL}) {
        Fixture f; assert(f.port.SendPrepare(1, true));
        f.Commit(generation); f.MustFail(); assert(!f.port.CommitSeen());
    }
    for (const auto enabled : {0U, 2U}) {
        Fixture f; assert(f.port.SendPrepare(1, true));
        f.Commit(1, enabled); f.MustFail(); assert(!f.port.CommitSeen());
    }
    {
        Fixture f; assert(f.port.SendPrepare(1, true)); f.Commit(1); assert(f.port.Poll());
        assert(f.port.CommitSeen() && f.port.CommitGeneration() == 1 && f.port.CommitEnabled());
        f.Commit(1); f.MustFail(); assert(f.port.CommitGeneration() == 1);
    }
    {
        Fixture f; assert(f.port.SendPrepare(1, true)); bool failed{};
        try { f.port.SendPrepare(2, false); } catch (const std::runtime_error&) { failed = true; }
        assert(failed);
    }
    {
        Fixture f; assert(f.port.PendingEmpty());
        f.Fragment(0, 2, false); assert(f.port.Poll()); assert(!f.port.PendingEmpty());
        f.Fragment(2, 2, true); assert(f.port.Poll()); assert(f.port.PendingEmpty());
    }
    {
        Fixture f;
        f.Fragment(0, 2, false, 4, 1); assert(f.port.Poll());
        f.Fragment(0, 2, false, 4, 3); assert(f.port.Poll());
        f.Fragment(2, 2, true, 4, 1); assert(f.port.Poll()); assert(!f.port.PendingEmpty());
        f.Fragment(2, 2, true, 4, 3); assert(f.port.Poll()); assert(f.port.PendingEmpty());
    }
    {
        Fixture f; f.Message(net_proto::MessageType::Frame).frame.length = UINT32_MAX;
        f.MustFail(); assert(f.port.PendingEmpty());
    }
    for (const auto variant : {0, 1, 2, 3}) {
        Fixture f;
        if (variant == 0) f.Fragment(0, 2, true);
        if (variant == 1) f.Fragment(0, 4, false);
        if (variant == 2) f.Fragment(2, 2, true);
        if (variant == 3) f.Fragment(UINT32_MAX, 1, true);
        f.MustFail(); assert(f.port.PendingEmpty());
    }
    for (const auto variant : {0, 1, 2}) {
        Fixture f; f.Fragment(0, 2, false); assert(f.port.Poll());
        if (variant == 0) f.Fragment(1, 3, true);
        if (variant == 1) f.Fragment(0, 2, false);
        if (variant == 2) f.Fragment(2, 3, true, 5);
        f.MustFail(); assert(!f.port.PendingEmpty());
    }
    for (const auto bad_version : {false, true}) {
        Fixture f; f.Fragment(0, 4, true);
        auto* header = reinterpret_cast<net_proto::UdmaWireHeader*>(
            reinterpret_cast<std::uint8_t*>(f.incoming.data()) + sizeof(net_proto::Message));
        if (bad_version) ++header->version; else ++header->payload_length;
        f.MustFail(); assert(f.port.PendingEmpty());
    }
}
