// SPDX-License-Identifier: Apache-2.0
#include "udma_model_internal.h"

namespace ubsim::device {
using namespace detail;

void
UdmaModel::ProcessSq(std::uint32_t jetty_id, std::uint32_t producer,
                     std::vector<std::uint8_t> direct)
{
    const auto found = jetty_contexts_.find(jetty_id);
    if (found == jetty_contexts_.end()) return;
    auto& slot = sq_pipelines_[jetty_id];
    if (!slot) {
        slot = std::make_shared<SqPipeline>();
        slot->issue = found->second.consumer;
        slot->next_fetch = slot->issue;
    }
    const auto state = slot;
    if (static_cast<std::int32_t>(producer - found->second.target_producer) > 0)
        found->second.target_producer = producer;
    if (!direct.empty() && direct.size() == abi::kWqebbBytes) {
        std::copy(direct.begin(), direct.end(), found->second.direct_wqe.begin());
        found->second.direct_pending = true;
    }
    state->kick = true;
    if (state->dispatching) return;
    state->dispatching = true;
    while (state->kick) {
        state->kick = false;
        ProcessSqOnce(jetty_id, producer, {});
    }
    state->dispatching = false;
}

void
UdmaModel::ProcessSqOnce(std::uint32_t jetty_id, std::uint32_t producer,
                         std::vector<std::uint8_t> direct)
{
    if (draining_queue_command_ == 0x07 && draining_queue_id_ == jetty_id) {
        MaybeCompleteQueueDestroy();
        return;
    }
    auto found = jetty_contexts_.find(jetty_id);
    if (found == jetty_contexts_.end()) return;
    QueueContext& jetty = found->second;
    // Doorbells may be coalesced while an earlier SQ DMA is still in flight.
    // Never let the producer captured by that old asynchronous callback move
    // the target backwards and hide newer work.  The signed delta is the
    // conventional modulo-2^32 sequence comparison and remains correct as
    // long as the queue has fewer than 2^31 outstanding WQEBBs.
    if (static_cast<std::int32_t>(producer - jetty.target_producer) > 0)
        jetty.target_producer = producer;
    const std::uint32_t target_producer = jetty.target_producer;
    if (!direct.empty()) {
        if (direct.size() != abi::kWqebbBytes) return;
        std::copy(direct.begin(), direct.end(), jetty.direct_wqe.begin());
        jetty.direct_pending = true;
    }
    const auto state = sq_pipelines_.at(jetty_id);
    if (state->fetching || state->ordered || state->prefetch_failed ||
        state->prepared_writes.size() >= config_.write_payload_read_window) return;
    if (static_cast<std::uint32_t>(state->issue) == jetty.target_producer) {
        jetty.direct_pending = false;
        return;
    }
    const std::uint32_t outstanding =
        jetty.target_producer - static_cast<std::uint32_t>(jetty.consumer);
    if (!jetty.depth || outstanding > jetty.depth) {
        ++sq_depth_rejects_;
        return;
    }
    if (jetty.direct_pending && static_cast<std::uint32_t>(state->issue + 1) == jetty.target_producer) {
        jetty.busy = true;
        state->fetching = true;
        jetty.direct_pending = false;
        HandleSqWqe(jetty_id, target_producer,
            std::vector<std::uint8_t>(jetty.direct_wqe.begin(),
                                      jetty.direct_wqe.end()));
        return;
    }
    FillSqCache(jetty_id);
    if (state->prefetch_failed) return;
    const auto head = state->cache.find(state->issue);
    if (head == state->cache.end() || head->second.empty()) return;
    std::array<std::uint8_t, abi::kWqebbBytes> first{};
    std::copy(head->second.begin(), head->second.end(), first.begin());
    const auto wqebbs = abi::SqWqe(first).wqebb_count();
    if (!wqebbs || wqebbs > target_producer - static_cast<std::uint32_t>(state->issue) ||
        wqebbs > jetty.depth) {
        state->prefetch_failed = true;
        ++sq_decode_rejects_;
        ++ubase_errors_;
        return;
    }
    std::vector<std::uint8_t> raw = head->second;
    if (wqebbs <= config_.sq_prefetch_wqebbs) {
        for (std::uint32_t i = 1; i < wqebbs; ++i) {
            auto part = state->cache.find(state->issue + i);
            if (part == state->cache.end() || part->second.empty()) return;
            raw.insert(raw.end(), part->second.begin(), part->second.end());
        }
        sq_prefetch_cache_hits_ += wqebbs;
        state->fetching = true;
        jetty.busy = true;
        HandleSqWqe(jetty_id, target_producer, std::move(raw));
    } else {
        // A legal WQE larger than the configured cache still uses the existing
        // bounded continuation path. Never increase the configured cache budget.
        ++sq_prefetch_cache_hits_;
        state->fetching = true;
        jetty.busy = true;
        ContinueSqWqe(jetty_id, target_producer, std::move(raw), wqebbs - 1,
                     (state->issue + 1) & (jetty.depth - 1U));
    }
}

void
UdmaModel::FillSqCache(std::uint32_t id)
{
    const auto state = sq_pipelines_.at(id);
    auto& queue = jetty_contexts_.at(id);
    state->next_fetch = std::max(state->next_fetch, state->issue);
    while (!state->prefetch_failed &&
           state->prefetch_inflight < config_.sq_fetch_max_outstanding &&
           state->cache.size() < config_.sq_prefetch_wqebbs) {
        const auto available = queue.target_producer - static_cast<std::uint32_t>(state->next_fetch);
        if (!available || available > queue.depth) break;
        // With a full cache, retiring one WQE opens just one slot. Fetching
        // immediately would turn a 16-WQEBB burst into a 64-byte DMA for
        // nearly every subsequent WQE. The cached head can keep issuing
        // while room accumulates; refill in bursts when enough slots open.
        const auto free_slots = config_.sq_prefetch_wqebbs - state->cache.size();
        if (free_slots < config_.sq_fetch_burst_wqebbs &&
            available >= config_.sq_fetch_burst_wqebbs &&
            state->cache.find(state->issue) != state->cache.end()) break;
        const auto start = state->next_fetch;
        const auto slot = start & (queue.depth - 1U);
        const auto count = std::min<std::uint32_t>({available,
            config_.sq_fetch_burst_wqebbs, queue.depth - static_cast<std::uint32_t>(slot),
            config_.sq_prefetch_wqebbs - static_cast<std::uint32_t>(state->cache.size())});
        for (std::uint32_t i = 0; i < count; ++i) state->cache.emplace(start + i, std::vector<std::uint8_t>{});
        state->next_fetch += count;
        ++state->prefetch_inflight;
        queue.busy = true;
        ++sq_dma_reads_;
        ++sq_prefetch_dma_reads_;
        sq_prefetch_dma_bytes_ += count * abi::kWqebbBytes;
        sq_prefetch_inflight_peak_ = std::max<std::uint64_t>(sq_prefetch_inflight_peak_, state->prefetch_inflight);
        sq_prefetch_cache_peak_ = std::max<std::uint64_t>(sq_prefetch_cache_peak_, state->cache.size());
        const auto prefetch_start = now_ps_;
        sq_prefetch_timing_.Begin();
        const auto prefetch_token = queue.token;
        const auto prefetch_iova = queue.queue_iova + slot * abi::kWqebbBytes;
        ReadToken(prefetch_token, prefetch_iova,
                  count * abi::kWqebbBytes,
            [this, id, state, start, count, prefetch_start, prefetch_token,
             prefetch_iova](bool ok, std::vector<std::uint8_t> raw) {
                sq_prefetch_timing_.End(prefetch_start, now_ps_);
                --state->prefetch_inflight;
                if (!ok || raw.size() != count * abi::kWqebbBytes) {
                    std::cerr << "udma-model: SQ prefetch failed"
                              << " jetty=" << id << " token=" << prefetch_token
                              << " iova=0x" << std::hex << prefetch_iova
                              << std::dec << " count=" << count
                              << " bytes=" << raw.size() << '\n';
                    state->prefetch_failed = true;
                    ++ubase_errors_;
                }
                if (!state->prefetch_failed) {
                    for (std::uint32_t i = 0; i < count; ++i) {
                        auto entry = state->cache.find(start + i);
                        if (entry != state->cache.end())
                            entry->second.assign(raw.begin() + i * abi::kWqebbBytes,
                                                 raw.begin() + (i + 1) * abi::kWqebbBytes);
                    }
                }
                const auto found = jetty_contexts_.find(id);
                if (found == jetty_contexts_.end()) return;
                found->second.busy = state->fetching || state->prefetch_inflight || !state->completions.empty();
                const auto producer = found->second.target_producer;
                MaybeCompleteQueueDestroy();
                if (!state->prefetch_failed) ProcessSq(id, producer);
            });
        // An immediate callback may complete a pending destroy.
        if (jetty_contexts_.find(id) == jetty_contexts_.end()) return;
    }
}

void
UdmaModel::ContinueSqWqe(std::uint32_t jetty_id, std::uint32_t producer,
                         std::vector<std::uint8_t> raw,
                         std::uint32_t remaining_wqebbs,
                         std::uint32_t next_slot)
{
    if (!remaining_wqebbs) {
        HandleSqWqe(jetty_id, producer, std::move(raw));
        return;
    }
    auto found = jetty_contexts_.find(jetty_id);
    if (found == jetty_contexts_.end()) return;
    const QueueContext& jetty = found->second;
    const std::uint32_t contiguous = std::min(remaining_wqebbs,
                                               jetty.depth - next_slot);
    const std::uint64_t address = jetty.queue_iova +
        std::uint64_t(next_slot) * abi::kWqebbBytes;
    ReadToken(jetty.token, address,
              std::size_t(contiguous) * abi::kWqebbBytes,
        [this, jetty_id, producer, raw = std::move(raw),
         remaining_wqebbs, next_slot, contiguous]
        (bool ok, std::vector<std::uint8_t> continuation) mutable {
            if (!ok || continuation.size() !=
                    std::size_t(contiguous) * abi::kWqebbBytes) {
                sq_pipelines_.at(jetty_id)->prefetch_failed = true;
                ReleaseSqFetch(jetty_id, false);
                ++ubase_errors_;
                MaybeCompleteQueueDestroy();
                return;
            }
            raw.insert(raw.end(), continuation.begin(), continuation.end());
            auto found = jetty_contexts_.find(jetty_id);
            if (found == jetty_contexts_.end()) return;
            ContinueSqWqe(jetty_id, producer, std::move(raw),
                          remaining_wqebbs - contiguous,
                          (next_slot + contiguous) &
                              (found->second.depth - 1U));
        });
}

void
UdmaModel::HandleSqWqe(std::uint32_t jetty_id, std::uint32_t producer,
                       std::vector<std::uint8_t> raw)
{
    auto fail = [this, jetty_id]() {
        auto state = sq_pipelines_.find(jetty_id);
        if (state != sq_pipelines_.end()) state->second->prefetch_failed = true;
        ReleaseSqFetch(jetty_id, false);
        ++ubase_errors_;
        MaybeCompleteQueueDestroy();
    };
    auto found = jetty_contexts_.find(jetty_id);
    if (found == jetty_contexts_.end() || raw.size() < abi::kWqebbBytes)
        return fail();
    std::array<std::uint8_t, abi::kWqebbBytes> bytes{};
    std::copy_n(raw.begin(), bytes.size(), bytes.begin());
    const abi::SqWqe wqe(bytes);
    QueueContext& jetty = found->second;
    const auto state = sq_pipelines_.at(jetty_id);
    if (state->prefetch_failed) {
        ReleaseSqFetch(jetty_id, false);
        return;
    }
    const bool expected_owner = ((state->issue / jetty.depth) & 1U) == 0;
    const std::uint32_t decoded_wqebbs = wqe.wqebb_count();
    const bool owner_valid = wqe.owner() == expected_owner;
    const bool length_valid =
        raw.size() == std::size_t(decoded_wqebbs) * abi::kWqebbBytes;
    const bool opcode_valid = wqe.opcode() == 0 || wqe.opcode() == 1 ||
        wqe.opcode() == 3 || wqe.opcode() == 6;
    if (!owner_valid || !length_valid || !opcode_valid) {
        ++sq_decode_rejects_;
        std::cerr << "udma-model: rejected SQ WQE header"
                  << " jetty=" << jetty_id
                  << " consumer=" << jetty.consumer
                  << " producer=" << producer
                  << " depth=" << jetty.depth
                  << " owner=" << wqe.owner()
                  << " expected_owner=" << expected_owner
                  << " opcode=" << unsigned(wqe.opcode())
                  << " flags=0x" << std::hex << unsigned(wqe.flags())
                  << std::dec
                  << " inline_len=" << wqe.inline_length()
                  << " sges=" << unsigned(wqe.sge_count())
                  << " raw_bytes=" << raw.size()
                  << " decoded_wqebbs=" << decoded_wqebbs
                  << '\n';
        return fail();
    }
    // Strong placement and fenced operations cannot pass older requests.
    // Retry this descriptor when the older completion retires.
    const bool ordered = wqe.place_order() == 2 || (wqe.flags() & 8U);
    // A later READ, SEND or inline WRITE must not overtake an earlier WRITE
    // whose payload DMA has not yet produced a frame. Non-inline WRITEs may
    // prepare concurrently; DrainPreparedWrites emits their frames in order.
    if (!state->prepared_writes.empty() &&
        (wqe.opcode() != 3 || wqe.inline_payload())) {
        ReleaseSqFetch(jetty_id, false);
        return;
    }
    if (ordered && !state->completions.empty()) {
        ReleaseSqFetch(jetty_id, false);
        return;
    }
    if (wqe.opcode() == 6 && config_.max_read_outstanding &&
        pending_rma_.size() >= config_.max_read_outstanding) {
        ++read_window_stalls_;
        ReleaseSqFetch(jetty_id, false);
        return;
    }
    ++sq_wqes_;
    const auto route = tp_routes_.find(wqe.tpn());
    if (route == tp_routes_.end() || !route->second.active ||
        route->second.port >= link_up_.size() ||
        !link_up_[route->second.port]) {
        std::cerr << "udma-model: rejected SQ route found="
                  << (route != tp_routes_.end())
                  << " active=" << (route != tp_routes_.end() && route->second.active)
                  << " jetty=" << jetty_id
                  << " consumer=" << jetty.consumer
                  << " producer=" << producer
                  << " depth=" << jetty.depth
                  << " tpn=" << wqe.tpn()
                  << " opcode=" << unsigned(wqe.opcode())
                  << " owner=" << wqe.owner()
                  << " inline_len=" << wqe.inline_length()
                  << " wqebbs=" << wqe.wqebb_count()
                  << '\n';
        return fail();
    }
    if (wqe.opcode() == 6) {
        if (wqe.inline_payload() || wqe.sge_count() != 1) return fail();
        const abi::Sge local = wqe.first_sge();
        if (!local.address || !local.length || !wqe.remote_address()) return fail();
        const std::uint64_t request_id = next_rma_request_++;
        // The official provider interprets CQE entry_idx as the last WQEBB
        // consumed by the completed WQE, then advances CI to entry_idx + 1.
        const std::uint16_t completed_index = static_cast<std::uint16_t>(
            (state->issue + decoded_wqebbs - 1U) & (jetty.depth - 1U));
        ReserveSq(jetty_id, decoded_wqebbs, ordered);
        pending_rma_[request_id] = PendingRma{
            jetty_id, producer, decoded_wqebbs, completed_index,
            wqe.opcode(), local.length,
            0, wqe.completion(), jetty.payload_token, local.address, 0, {}};
        rma_pending_peak_ = std::max<std::uint64_t>(rma_pending_peak_, pending_rma_.size());
        Frame frame{};
        frame.sequence = next_sequence_++;
        frame.source_eid = config_.endpoint_eid + jetty.eid_index * 0x10000U;
        frame.destination_eid = route->second.remote_eid;
        frame.source_port = static_cast<std::uint16_t>(route->second.port);
        frame.destination_port = UINT16_MAX;
        frame.operation = Frame::Operation::ReadRequest;
        frame.order_type = wqe.place_order();
        frame.source_jetty = jetty_id;
        frame.destination_jetty = wqe.remote_jetty();
        frame.tpn = wqe.tpn();
        frame.segment = wqe.remote_segment();
        frame.remote_address = wqe.remote_address();
        frame.request_id = request_id;
        frame.transfer_length = local.length;
        network_.Send(std::move(frame), [this, request_id, jetty_id](bool ok) {
            if (ok) return;
            const auto request = pending_rma_.find(request_id);
            if (request == pending_rma_.end()) { ++ubase_errors_; return; }
            request->second.failed = true;
            if (request->second.response_dma) { ++ubase_errors_; return; }
            const auto pending = request->second;
            pending_rma_.erase(request_id);
            CompleteSq(jetty_id, pending.producer, pending.wqebbs,
                       pending.completed_index, pending.opcode, pending.byte_count,
                       pending.immediate, pending.completion, 1);
            ++ubase_errors_;
            MaybeCompleteQueueDestroy();
        });
        ReleaseSqFetch(jetty_id, true);
        return;
    }
    // SEND is a single transport message limited to 4096 bytes. Reject its
    // descriptor before reading payload or entering the network adapter.
    // Official UDMA ABI: LOCAL_OP_ERR (2), LOCAL_LENGTH_ERR (1), mapped by
    // udma_u_get_cr_status() to URMA_CR_LOC_LEN_ERR (not raw status 1).
    if ((wqe.opcode() == 0 || wqe.opcode() == 1) &&
        (wqe.inline_payload() || wqe.sge_count() == 1)) {
        const auto length = wqe.inline_payload() ? wqe.inline_length() :
            wqe.first_sge().length;
        if (length > 4096) {
            const auto index = static_cast<std::uint16_t>(
                (state->issue + decoded_wqebbs - 1U) & (jetty.depth - 1U));
            ReserveSq(jetty_id, decoded_wqebbs, ordered);
            CompleteSq(jetty_id, producer, decoded_wqebbs, index,
                wqe.opcode(), length, wqe.immediate(), wqe.completion(), 2, 1);
            ReleaseSqFetch(jetty_id, true);
            return;
        }
    }
    if (wqe.inline_payload()) {
        if (wqe.inline_length() > raw.size() - 48) return fail();
        std::vector<std::uint8_t> payload(
            raw.begin() + 48, raw.begin() + 48 + wqe.inline_length());
        SubmitSqPayload(jetty_id, producer, std::move(raw),
                        std::move(payload));
        return;
    }
    if (wqe.sge_count() != 1) return fail();
    const abi::Sge sge = wqe.first_sge();
    if (!sge.address || !sge.length) return fail();
    if (wqe.opcode() == 3) {
        const auto completed_index = static_cast<std::uint16_t>(
            (state->issue + decoded_wqebbs - 1U) & (jetty.depth - 1U));
        auto prepared = std::make_shared<SqPipeline::PreparedWrite>();
        prepared->producer = producer;
        prepared->wqebbs = decoded_wqebbs;
        prepared->completed_index = completed_index;
        prepared->byte_count = sge.length;
        prepared->immediate = wqe.immediate();
        prepared->completion_enabled = wqe.completion();
        prepared->raw = std::move(raw);
        state->prepared_writes.push_back(prepared);
        ReserveSq(jetty_id, decoded_wqebbs, ordered);
        const auto payload_start = now_ps_;
        source_payload_timing_.Begin();
        ReadToken(jetty.payload_token, sge.address, sge.length,
            [this, jetty_id, prepared, expected = sge.length,
             payload_start](bool ok, std::vector<std::uint8_t> payload) {
                source_payload_timing_.End(payload_start, now_ps_);
                prepared->failed = !ok || payload.size() != expected;
                if (!prepared->failed) prepared->payload = std::move(payload);
                prepared->ready = true;
                DrainPreparedWrites(jetty_id);
            }, config_.dma_max_outstanding);
        ReleaseSqFetch(jetty_id, true);
        return;
    }
    ReadToken(jetty.payload_token, sge.address, sge.length,
        [this, jetty_id, producer, raw = std::move(raw), expected = sge.length]
        (bool ok, std::vector<std::uint8_t> payload) mutable {
            if (!ok || payload.size() != expected) {
                sq_pipelines_.at(jetty_id)->prefetch_failed = true;
                ReleaseSqFetch(jetty_id, false);
                ++ubase_errors_;
                MaybeCompleteQueueDestroy();
                return;
            }
            SubmitSqPayload(jetty_id, producer, std::move(raw),
                            std::move(payload));
        }, config_.dma_max_outstanding);
}

void
UdmaModel::DrainPreparedWrites(std::uint32_t jetty_id)
{
    const auto found = sq_pipelines_.find(jetty_id);
    if (found == sq_pipelines_.end()) return;
    const auto state = found->second;
    while (!state->prepared_writes.empty() &&
           state->prepared_writes.front()->ready) {
        auto prepared = std::move(state->prepared_writes.front());
        state->prepared_writes.pop_front();
        if (prepared->failed) {
            ++ubase_errors_;
            CompleteSq(jetty_id, prepared->producer, prepared->wqebbs,
                       prepared->completed_index, 3, prepared->byte_count,
                       prepared->immediate, prepared->completion_enabled, 2);
            continue;
        }
        SubmitSqPayload(jetty_id, prepared->producer, std::move(prepared->raw),
                        std::move(prepared->payload), prepared->completed_index);
    }
    const auto jetty = jetty_contexts_.find(jetty_id);
    if (jetty != jetty_contexts_.end())
        ProcessSq(jetty_id, jetty->second.target_producer);
    MaybeCompleteQueueDestroy();
}

void
UdmaModel::SubmitSqPayload(std::uint32_t jetty_id, std::uint32_t producer,
                           std::vector<std::uint8_t> raw,
                           std::vector<std::uint8_t> payload,
                           std::optional<std::uint16_t> reserved_index)
{
    auto found = jetty_contexts_.find(jetty_id);
    if (found == jetty_contexts_.end()) return;
    if (!reserved_index && sq_pipelines_.at(jetty_id)->prefetch_failed) {
        ReleaseSqFetch(jetty_id, false);
        return;
    }
    std::array<std::uint8_t, abi::kWqebbBytes> bytes{};
    if (raw.size() < bytes.size()) return;
    std::copy_n(raw.begin(), bytes.size(), bytes.begin());
    const abi::SqWqe wqe(bytes);
    const std::uint32_t wqebbs = wqe.wqebb_count();
    const QueueContext jetty = found->second;
    const auto route = tp_routes_.find(wqe.tpn());
    if (route == tp_routes_.end()) return;
    Frame frame{};
    frame.sequence = next_sequence_++;
    frame.source_eid = config_.endpoint_eid + jetty.eid_index * 0x10000U;
    frame.destination_eid = route->second.remote_eid;
    frame.source_port = static_cast<std::uint16_t>(route->second.port);
    frame.destination_port = UINT16_MAX;
    frame.operation = wqe.opcode() == 0 ? Frame::Operation::Send :
        (wqe.opcode() == 1 ? Frame::Operation::SendImmediate :
                             Frame::Operation::Write);
    frame.order_type = wqe.place_order();
    frame.source_jetty = jetty_id;
    frame.destination_jetty = wqe.remote_jetty();
    frame.tpn = wqe.tpn();
    frame.immediate = wqe.immediate();
    frame.segment = wqe.remote_segment();
    frame.remote_address = wqe.remote_address();
    frame.bytes = std::move(payload);
    const std::uint32_t count = static_cast<std::uint32_t>(frame.bytes.size());
    // CQE entry_idx names the final WQEBB, not the first one.  This matters
    // for inline WQEs spanning two or more blocks: the provider reconstructs
    // its SQ consumer as entry_idx + 1.
    const std::uint16_t completed_index = reserved_index ? *reserved_index :
        static_cast<std::uint16_t>((sq_pipelines_.at(jetty_id)->issue + wqebbs - 1U) &
                                   (jetty.depth - 1U));
    if (!reserved_index)
        ReserveSq(jetty_id, wqebbs, wqe.place_order() == 2 || (wqe.flags() & 8U));
    std::uint64_t request_id = 0;
    if (wqe.opcode() == 3) {
        request_id = next_rma_request_++;
        frame.request_id = request_id;
        frame.transfer_length = count;
        pending_rma_[request_id] = PendingRma{
            jetty_id, producer, wqebbs, completed_index, wqe.opcode(), count,
            wqe.immediate(), wqe.completion(), 0, 0, 0, {}};
        rma_pending_peak_ = std::max<std::uint64_t>(rma_pending_peak_, pending_rma_.size());
        write_ack_starts_[request_id] = now_ps_;
        write_ack_timing_.Begin();
    }
    network_.Send(std::move(frame),
        [this, jetty_id, producer, completed_index, opcode = wqe.opcode(),
         wqebbs,
         count, immediate = wqe.immediate(), completion = wqe.completion(),
         request_id]
        (bool ok) {
            if (!ok) {
                if (request_id) {
                    pending_rma_.erase(request_id);
                    const auto started = write_ack_starts_.find(request_id);
                    if (started != write_ack_starts_.end()) {
                        write_ack_timing_.End(started->second, now_ps_);
                        write_ack_starts_.erase(started);
                    }
                }
                CompleteSq(jetty_id, producer, wqebbs, completed_index, opcode,
                           count, immediate, completion, 1);
                ++ubase_errors_;
                MaybeCompleteQueueDestroy();
                return;
            }
            if (opcode == 3) return;
            CompleteSq(jetty_id, producer, wqebbs, completed_index, opcode, count,
                       immediate, completion);
        });
    if (!reserved_index) ReleaseSqFetch(jetty_id, true);
}

void
UdmaModel::ReserveSq(std::uint32_t id, std::uint32_t wqebbs, bool ordered)
{
    const auto state = sq_pipelines_.at(id);
    const auto depth = jetty_contexts_.at(id).depth;
    SqCompletion entry{};
    entry.wqebbs = wqebbs;
    entry.index = static_cast<std::uint16_t>((state->issue + wqebbs - 1) & (depth - 1));
    state->completions.push_back(entry);
    for (std::uint32_t i = 0; i < wqebbs; ++i) state->cache.erase(state->issue + i);
    state->issue += wqebbs;
    state->ordered = ordered;
}

void
UdmaModel::ReleaseSqFetch(std::uint32_t id, bool resume)
{
    const auto found = jetty_contexts_.find(id);
    const auto pipeline = sq_pipelines_.find(id);
    if (found == jetty_contexts_.end() || pipeline == sq_pipelines_.end()) return;
    pipeline->second->fetching = false;
    found->second.busy = pipeline->second->prefetch_inflight || !pipeline->second->completions.empty();
    const auto producer = found->second.target_producer;
    MaybeCompleteQueueDestroy();
    if (resume) ProcessSq(id, producer);
}

void
UdmaModel::CompleteSq(std::uint32_t jetty_id, std::uint32_t /*producer*/,
                      std::uint32_t wqebbs, std::uint16_t completed_index,
                      std::uint8_t opcode, std::uint32_t byte_count,
                      std::uint64_t immediate, bool completion_enabled,
                      std::uint8_t status, std::uint8_t substatus)
{
    const auto pipeline = sq_pipelines_.find(jetty_id);
    if (pipeline == sq_pipelines_.end()) { ++ubase_errors_; return; }
    // No WQEBB slot can be reused before retirement, so its final ring index
    // identifies exactly one entry in this bounded, unretired window.
    auto& entries = pipeline->second->completions;
    const auto entry = std::find_if(entries.begin(), entries.end(),
        [completed_index](const auto& e) { return e.index == completed_index; });
    if (entry == entries.end() || entry->ready || entry->wqebbs != wqebbs) {
        ++ubase_errors_;
        return;
    }
    *entry = SqCompletion{wqebbs, completed_index, opcode, byte_count,
                          immediate, completion_enabled, true, status, substatus};
    RetireSq(jetty_id);
}

void
UdmaModel::RetireSq(std::uint32_t id)
{
    if (config_.pipelined_completion_dma) return RetireSqPipelined(id);
    const auto state = sq_pipelines_.at(id);
    if (state->retiring) return;
    state->retiring = true;
    while (!state->publishing && !state->completions.empty() &&
           state->completions.front().ready) {
        const auto entry = state->completions.front();
        const auto found = jetty_contexts_.find(id);
        if (found == jetty_contexts_.end()) break;
        state->publishing = true;
        auto finish = [this, id, state, entry](bool ok) {
            if (!ok) {
                // Keep ownership on a failed CQ DMA: no successful CQE was
                // published and releasing its queue could cause use-after-free.
                ++ubase_errors_;
                return;
            }
            const auto found = jetty_contexts_.find(id);
            if (found == jetty_contexts_.end()) return;
            auto& queue = found->second;
            sq_consumer_wraps_ += ((queue.consumer % queue.depth) +
                                   std::uint64_t(entry.wqebbs)) / queue.depth;
            sq_wqebbs_retired_ += entry.wqebbs;
            queue.consumer += entry.wqebbs;
            ++sq_completions_;
            state->completions.pop_front();
            state->publishing = false;
            if (state->completions.empty()) state->ordered = false;
            queue.busy = state->fetching || state->prefetch_inflight || !state->completions.empty();
            const auto producer = queue.target_producer;
            RetireSq(id);
            MaybeCompleteQueueDestroy();
            ProcessSq(id, producer);
        };
        if (!entry.enabled) finish(true);
        else WriteCqe(found->second.completion_queue, false, found->second.is_jetty,
                      entry.opcode, entry.index, id, entry.bytes,
                      found->second.user_queue, entry.immediate, std::move(finish),
                      0, 0, 0, entry.status, entry.substatus);
    }
    state->retiring = false;
}

void
UdmaModel::RetireSqPipelined(std::uint32_t id)
{
    const auto state = sq_pipelines_.at(id);
    if (state->retiring || state->publication_failed) return;
    state->retiring = true;
    while (state->publications_in_flight < config_.completion_event_slots) {
        // Issue CQEs in SQ order. Their DMA callbacks may finish out of order,
        // but only the contiguous published prefix can retire WQEBB slots.
        auto next = state->completions.begin();
        while (next != state->completions.end() && next->ready &&
               next->publication_started) ++next;
        if (next == state->completions.end() || !next->ready) break;
        const auto jetty = jetty_contexts_.find(id);
        if (jetty == jetty_contexts_.end()) break;
        next->publication_started = true;
        const auto entry = *next;
        ++state->publications_in_flight;
        state->publishing = true;
        auto finish = [this, id, state, entry](bool ok) {
            --state->publications_in_flight;
            if (!ok) {
                // A later slot may already be visible. Do not reuse any SQ
                // slot or claim a clean checkpoint after a failed CQ DMA.
                state->publication_failed = true;
                state->prefetch_failed = true;
                ++ubase_errors_;
                return;
            }
            auto current = std::find_if(state->completions.begin(),
                state->completions.end(), [entry](const auto& candidate) {
                    return candidate.index == entry.index;
                });
            if (current == state->completions.end() ||
                !current->publication_started || current->publication_done) {
                state->publication_failed = true;
                state->prefetch_failed = true;
                ++ubase_errors_;
                return;
            }
            current->publication_done = true;
            const auto found = jetty_contexts_.find(id);
            if (found == jetty_contexts_.end()) return;
            auto& queue = found->second;
            while (!state->completions.empty() &&
                   state->completions.front().publication_done) {
                const auto front = state->completions.front();
                sq_consumer_wraps_ += ((queue.consumer % queue.depth) +
                                       std::uint64_t(front.wqebbs)) / queue.depth;
                sq_wqebbs_retired_ += front.wqebbs;
                queue.consumer += front.wqebbs;
                ++sq_completions_;
                state->completions.pop_front();
            }
            state->publishing = state->publications_in_flight != 0;
            if (state->completions.empty()) state->ordered = false;
            queue.busy = state->fetching || state->prefetch_inflight ||
                         !state->completions.empty();
            RetireSqPipelined(id);
            MaybeCompleteQueueDestroy();
            ProcessSq(id, queue.target_producer);
        };
        if (!entry.enabled) finish(true);
        else WriteCqe(jetty->second.completion_queue, false,
                      jetty->second.is_jetty, entry.opcode, entry.index,
                      id, entry.bytes, jetty->second.user_queue,
                      entry.immediate, std::move(finish), 0, 0, 0,
                      entry.status, entry.substatus);
        if (state->publication_failed) break;
    }
    state->retiring = false;
}

} // namespace ubsim::device
