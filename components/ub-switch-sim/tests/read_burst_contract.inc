// Native diagnostic only: symmetric, concurrent 64KiB READs with immediate
// correct target DMA. Deliberately no application cleanup on a source failure.
int ReadBurst(net::Interface& interface, bool side, unsigned count)
{
    if (!count || count > 2048) return 2;
    const std::uint32_t local = side ? 0x101 : 0x202;
    const std::uint32_t remote = side ? 0x202 : 0x101;
    std::uint64_t now = 0, first_error = 0, max_reply_dwell_ps = 0, outalloc_backpressure = 0;
    unsigned issued = 0, requests = 0, responses = 0, errors = 0, completed = 0;
    bool link = false;
    std::deque<net::UdmaWireHeader> replies;
    std::deque<std::uint64_t> reply_enqueued;
    std::set<std::pair<std::uint64_t, std::uint32_t>> target_seen, source_seen;
    std::vector<unsigned> returned(count, 0);
    std::vector<bool> terminal(count, false);
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(30)) {
        net::UbNetOutSync(&interface, now);
        // Prioritize real target callbacks, no artificial target service delay.
        if (!replies.empty() || (link && issued < count)) {
            auto* out = net::UbNetOutAlloc(&interface, now);
            if (out) {
                ZeroVolatile(out->frame);
                out->frame.source_eid = local;
                out->frame.destination_eid = remote;
                out->frame.source_port = 0;
                net::UdmaWireHeader wire{};
                if (!replies.empty()) {
                    wire = replies.front(); replies.pop_front();
                    max_reply_dwell_ps = std::max(max_reply_dwell_ps, now - reply_enqueued.front());
                    reply_enqueued.pop_front();
                    const auto source_jetty = wire.source_jetty;
                    wire.source_jetty = wire.destination_jetty;
                    wire.destination_jetty = source_jetty;
                    wire.operation = static_cast<std::uint8_t>(net::UdmaOperation::ReadResponse);
                    wire.flags = net::kUdmaWireCtpSegment | net::kUdmaWireLastFragment;
                    wire.payload_length = wire.transfer_length;
                    ++responses;
                } else {
                    wire.magic = net::kUdmaWireMagic; wire.version = net::kUdmaWireVersion;
                    wire.operation = static_cast<std::uint8_t>(net::UdmaOperation::ReadRequest);
                    wire.flags = net::kUdmaWireLastFragment;
                    wire.source_jetty = side ? 7 : 9;
                    wire.destination_jetty = side ? 9 : 7;
                    wire.segment = 11; wire.remote_address = 0x10000;
                    wire.request_id = ++issued; wire.transfer_length = 65536;
                }
                out->frame.sequence = issued + responses;
                out->frame.length = sizeof(wire) + wire.payload_length;
                auto* payload = reinterpret_cast<volatile std::uint8_t*>(out) + sizeof(net::Message);
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(&wire);
                for (std::size_t i = 0; i < sizeof(wire); ++i) payload[i] = bytes[i];
                for (unsigned i = 0; i < wire.payload_length; ++i) payload[sizeof(wire)+i] = 0x5a;
                net::UbNetOutSend(&interface, out, static_cast<std::uint8_t>(net::MessageType::Frame));
            } else ++outalloc_backpressure;
        }
        auto* in = net::UbNetInPoll(&interface, now);
        if (in) {
            const auto type = static_cast<net::MessageType>(net::UbNetInType(&interface, in));
            if (type == net::MessageType::LinkState) link = in->link.state == static_cast<std::uint8_t>(net::LinkState::Up);
            else if (type == net::MessageType::Frame) {
                if (in->frame.length < sizeof(net::UdmaWireHeader)) return 8;
                net::UdmaWireHeader wire{};
                auto* bytes = reinterpret_cast<std::uint8_t*>(&wire);
                const auto* payload = reinterpret_cast<const volatile std::uint8_t*>(in) + sizeof(net::Message);
                for (std::size_t i = 0; i < sizeof(wire); ++i) bytes[i] = payload[i];
                if (!wire.request_id || wire.request_id > count ||
                    wire.magic != net::kUdmaWireMagic || wire.version != net::kUdmaWireVersion ||
                    in->frame.length != sizeof(wire)+wire.payload_length) return 8;
                const auto op = static_cast<net::UdmaOperation>(wire.operation);
                const unsigned id = wire.request_id - 1;
                if (in->frame.source_eid != remote || in->frame.destination_eid != local ||
                    wire.source_jetty != (side ? 9U : 7U) ||
                    wire.destination_jetty != (side ? 7U : 9U)) {
                    std::cerr << "BURST_BAD_IDENTITY now_ps=" << now << std::endl;
                    return 8;
                }
                if (op != net::UdmaOperation::RmaError &&
                    (wire.payload_offset % 4096 || wire.payload_offset >= 65536 ||
                     (op == net::UdmaOperation::ReadRequest &&
                      (wire.segment != 11 || wire.remote_address != 0x10000 + wire.payload_offset)))) {
                    std::cerr << "BURST_BAD_RANGE now_ps=" << now << " op=" << unsigned(wire.operation)
                              << " offset=" << wire.payload_offset << " token=" << wire.segment
                              << " address=" << wire.remote_address << std::endl;
                    return 8;
                }
                if (op == net::UdmaOperation::ReadRequest) {
                    if (wire.transfer_length != 4096 || wire.payload_length ||
                        !target_seen.emplace(std::uint64_t(wire.request_id),
                                             std::uint32_t(wire.payload_offset)).second) {
                        std::cerr << "BURST_BAD_TARGET now_ps=" << now << " id=" << wire.request_id
                                  << " offset=" << wire.payload_offset << " length=" << wire.transfer_length << std::endl;
                        return 8;
                    }
                    ++requests; replies.push_back(wire); reply_enqueued.push_back(now);
                } else if (op == net::UdmaOperation::ReadResponse) {
                    if (wire.transfer_length != 4096 || wire.payload_length != 4096 ||
                        !source_seen.emplace(std::uint64_t(wire.request_id),
                                             std::uint32_t(wire.payload_offset)).second) {
                        std::cerr << "BURST_BAD_RESPONSE now_ps=" << now << " id=" << wire.request_id
                                  << " offset=" << wire.payload_offset << " length=" << wire.transfer_length << std::endl;
                        return 8;
                    }
                    for (unsigned i = 0; i < wire.payload_length; ++i)
                        if (payload[sizeof(wire)+i] != 0x5a) return 8;
                    if (++returned[id] == 16 && !terminal[id]) { terminal[id] = true; ++completed; }
                } else if (op == net::UdmaOperation::RmaError) {
                    if (terminal[id]) { std::cerr << "BURST_DUP_TERMINAL id=" << wire.request_id << std::endl; return 8; }
                    terminal[id] = true; ++errors;
                    if (!first_error) {
                        first_error = now;
                        std::cout << "BURST_FIRST_ERROR now_ps=" << now << " request=" << wire.request_id
                                  << " issued=" << issued << " target_requests=" << requests
                                  << " target_responses=" << responses << " received_tas=" << source_seen.size()
                                  << " completed=" << completed << std::endl;
                    }
                } else return 8;
            }
            net::UbNetInDone(&interface, in);
        } else {
            const auto next = std::min(net::UbNetInTimestamp(&interface), net::UbNetOutNextSync(&interface));
            if (next > now && next != std::numeric_limits<std::uint64_t>::max()) now = next;
            std::this_thread::yield();
        }
        // Fixed shared end time prevents one peer disconnecting early while the
        // other still receives data. 10ms is well beyond the native retry budget.
        if (now >= 10000000000ULL) {
            std::cout << "BURST_RESULT count=" << count << " issued=" << issued
                      << " completed=" << completed << " errors=" << errors
                      << " target_requests=" << requests << " target_responses=" << responses
                      << " received_tas=" << source_seen.size() << " pending_replies=" << replies.size()
                      << " max_reply_dwell_ps=" << max_reply_dwell_ps
                      << " outalloc_backpressure=" << outalloc_backpressure
                      << " first_error_ps=" << first_error << " now_ps=" << now << std::endl;
            return issued == count && completed+errors == count && replies.empty() ? 0 : 6;
        }
    }
    std::cerr << "BURST_WALL_TIMEOUT now_ps=" << now << std::endl;
    return 6;
}
