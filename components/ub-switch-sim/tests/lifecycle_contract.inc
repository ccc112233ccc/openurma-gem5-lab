// Idle native allocators retain periodic watchdogs across protocol epochs.
int LifecycleIdle(net::Interface& interface)
{
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t now = 0;
    unsigned generation = 1;
    bool prepared = false, active = false, link = false;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(20)) {
        if (active) net::UbNetOutSync(&interface, now);
        // Run beyond the native 10ms watchdog period during each enabled epoch.
        if (link && !prepared && (!active || now >= 12000000000ULL)) {
            auto* out = net::UbNetOutAlloc(&interface, now);
            if (out) {
                ZeroVolatile(out->lifecycle);
                out->lifecycle.generation = generation;
                out->lifecycle.action = static_cast<std::uint8_t>(net::LifecycleAction::PrepareSync);
                out->lifecycle.enabled = active ? 0 : 1;
                net::UbNetOutSend(&interface, out,
                    static_cast<std::uint8_t>(net::MessageType::Lifecycle));
                prepared = true;
            }
        }
        auto* in = net::UbNetInPoll(&interface, prepared ? UINT64_MAX : now);
        if (in) {
            const auto type = net::UbNetInType(&interface, in);
            if (type == static_cast<std::uint8_t>(net::MessageType::LinkState)) {
                link = true;
            } else if (type == static_cast<std::uint8_t>(net::MessageType::Lifecycle)) {
                if (!prepared || in->lifecycle.generation != generation ||
                    in->lifecycle.action != static_cast<std::uint8_t>(net::LifecycleAction::CommitSync) ||
                    in->lifecycle.enabled != (active ? 0 : 1)) return 8;
                net::UbNetInDone(&interface, in);
                active = !active;
                interface.base.sync = active;
                if (active) now = 0;
                interface.base.in_timestamp = interface.base.out_timestamp = now;
                std::cout << "LIFECYCLE_COMMIT generation=" << generation
                          << " active=" << active << std::endl;
                if (++generation == 5) {
                    std::cout << "LIFECYCLE_IDLE PASS" << std::endl;
                    return 0;
                }
                prepared = false;
                continue;
            }
            net::UbNetInDone(&interface, in);
        } else {
            if (active && !prepared) {
                const auto next = std::min(net::UbNetInTimestamp(&interface),
                                           net::UbNetOutNextSync(&interface));
                if (next > now && next != UINT64_MAX) now = next;
            }
            std::this_thread::yield();
        }
    }
    std::cerr << "LIFECYCLE_IDLE timeout generation=" << generation
              << " prepared=" << prepared << " now=" << now << std::endl;
    return 6;
}
