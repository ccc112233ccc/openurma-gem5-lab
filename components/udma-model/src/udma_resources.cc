// SPDX-License-Identifier: Apache-2.0
#include "udma_model_internal.h"

namespace ubsim::device {
using namespace detail;

void
UdmaModel::HandleUbaseMailbox(std::vector<std::uint8_t> descriptor,
                              std::uint32_t descriptor_count)
{
    const std::uint8_t command = descriptor[16];
    const std::uint64_t context_iova =
        LoadLe<std::uint32_t>(descriptor.data() + 8) |
        (std::uint64_t(LoadLe<std::uint32_t>(descriptor.data() + 12)) << 32);
    constexpr std::uint8_t QueryJfsContext = 0x06;
    constexpr std::uint8_t DestroyJfsContext = 0x07;
    constexpr std::uint8_t DestroyJfcContext = 0x27;
    constexpr std::uint8_t DestroyCeqContext = 0x47;
    constexpr std::uint8_t DestroyJfrContext = 0x57;
    const std::uint32_t tag =
        LoadLe<std::uint32_t>(descriptor.data() + 16) >> 8;

    // Official UBASE creates one AEQ (eqn 0); do not alias other IDs to it.
    if ((command == 0x34 || command == 0x37) && tag != 0)
        return RejectUbase(std::move(descriptor), descriptor_count, 95);
    const bool eq_busy = (command == 0x34 || command == 0x37) ?
        (aeq_in_flight_ != 0 || !mailbox_events_.empty()) : CeqBusy(tag);
    // Keep ownership of CSQ until old event writes and interrupts finish.
    // The official driver frees EQ memory even on mailbox failure, so EBUSY
    // is not a safe completion for destruction.
    if ((command == 0x37 || command == 0x47) && eq_busy) {
        draining_eq_command_ = command;
        draining_eq_id_ = tag;
        pending_eq_destroy_ = [this, descriptor = std::move(descriptor),
                               descriptor_count]() mutable {
            HandleUbaseMailbox(std::move(descriptor), descriptor_count);
        };
        return;
    }
    if ((command == 0x34 || command == 0x44) && eq_busy)
        return RejectUbase(std::move(descriptor), descriptor_count, 16);

    if (command == DestroyJfcContext && JfcPublicationBusy(tag)) {
        draining_jfc_id_ = tag;
        pending_jfc_destroy_ = [this, descriptor = std::move(descriptor),
                                descriptor_count]() mutable {
            HandleUbaseMailbox(std::move(descriptor), descriptor_count);
        };
        return;
    }

    const bool needs_context = command == QueryJfsContext || command == 0x26 || command == 0x34 ||
        command == 0x44 || command == 0x04 || command == 0x24 ||
        command == 0x54 || command == 0x25;
    if (needs_context && !context_iova)
        return RejectUbase(std::move(descriptor), descriptor_count, 22);
    if (command == QueryJfsContext && !jetty_contexts_.count(tag))
        return RejectUbase(std::move(descriptor), descriptor_count, 2);
    if (command == 0x26 && !jfc_query_contexts_.count(tag))
        return RejectUbase(std::move(descriptor), descriptor_count, 2);
    if ((command == 0x04 && jetty_contexts_.count(tag)) ||
        (command == 0x24 && jfc_contexts_.count(tag)) ||
        (command == 0x54 && jfr_contexts_.count(tag)))
        return RejectUbase(std::move(descriptor), descriptor_count, 17);
    // JFS and duplex Jetty share the UDMA SQ identity pool. The guest driver
    // normally rejects exhaustion first, but a raw mailbox must not bypass
    // the device's advertised resource bound.
    if (command == 0x04 && jetty_contexts_.size() >= config_.max_jettys)
        return RejectUbase(std::move(descriptor), descriptor_count, 28);

    if ((command == DestroyJfsContext || command == DestroyJfrContext) &&
        ResourceQueueBusy(command, tag)) {
        draining_queue_command_ = command;
        draining_queue_id_ = tag;
        pending_queue_destroy_ = [this, descriptor = std::move(descriptor),
                                  descriptor_count]() mutable {
            HandleUbaseMailbox(std::move(descriptor), descriptor_count);
        };
        return;
    }
    if (command == DestroyJfsContext || command == DestroyJfrContext) {
        // Queued packets have not acquired resource ownership yet. Never
        // deliver an old packet to a resource re-created with the same ID.
        receive_frames_.erase(std::remove_if(receive_frames_.begin(),
            receive_frames_.end(), [this, command, tag](const Frame& frame) {
                if (command == 0x07) return frame.destination_jetty == tag;
                const auto jetty = jetty_contexts_.find(frame.destination_jetty);
                return jetty != jetty_contexts_.end() &&
                    jetty->second.receive_queue == tag;
            }), receive_frames_.end());
    }
    if (command == DestroyJfsContext) {
        jetty_contexts_.erase(tag);
        sq_pipelines_.erase(tag);
    }
    if (command == DestroyJfcContext) {
        if (jfc_contexts_.count(tag))
            jfc_query_contexts_[tag] = BuildJfcQueryContext(tag);
        jfc_contexts_.erase(tag);
        jfc_ceqs_.erase(tag);
        armed_jfcs_.erase(tag);
        jfc_cqe_dma_in_flight_.erase(tag);
    }
    if (command == DestroyJfrContext) jfr_contexts_.erase(tag);
    if (command == 0x37) {
        aeq_iova_ = 0;
        aeq_depth_ = 0;
        aeq_producer_ = 0;
    }
    if (command == DestroyCeqContext) {
        ceqs_.erase(tag);
    }

    if (command == 0x26) {
        QueryJfcContext(tag,
            [this, context_iova, descriptor = std::move(descriptor), descriptor_count]
            (bool ok, std::vector<std::uint8_t> context) mutable {
                if (!ok) return RejectUbase(std::move(descriptor), descriptor_count, 5);
                WriteIoVirtual(context_iova, std::move(context),
                    [this, descriptor = std::move(descriptor), descriptor_count](bool written) mutable {
                        if (!written) return RejectUbase(std::move(descriptor), descriptor_count, 5);
                        ApplyUbaseMailbox(std::move(descriptor), descriptor_count, {});
                    });
            });
        return;
    }

    if (command == QueryJfsContext && context_iova) {
        std::vector<std::uint8_t> context(128, 0);
        StoreLe<std::uint32_t>(context, 0, 1U << 16);
        StoreLe<std::uint32_t>(context, 26 * sizeof(std::uint32_t), 1U << 26);
        WriteIoVirtual(context_iova, std::move(context),
            [this, descriptor = std::move(descriptor), descriptor_count](bool ok) mutable {
                if (!ok) return RejectUbase(std::move(descriptor), descriptor_count, 5);
                ApplyUbaseMailbox(std::move(descriptor), descriptor_count, {});
            });
        return;
    }

    const bool creates_context = command == 0x34 || command == 0x44 ||
        command == 0x04 || command == 0x24 || command == 0x54;
    const bool modifies_jfc = command == 0x25;
    if ((creates_context || modifies_jfc) && context_iova) {
        const std::size_t bytes = modifies_jfc ? 256 :
            ((command == 0x34 || command == 0x44) ? 64 : 128);
        ReadIoVirtual(context_iova, bytes,
            [this, descriptor = std::move(descriptor), descriptor_count, bytes]
            (bool ok, std::vector<std::uint8_t> context) mutable {
                if (!ok || context.size() != bytes)
                    return RejectUbase(std::move(descriptor), descriptor_count, 5);
                ApplyUbaseMailbox(std::move(descriptor), descriptor_count,
                                  std::move(context));
            });
        return;
    }
    ApplyUbaseMailbox(std::move(descriptor), descriptor_count, {});
}

void
UdmaModel::ApplyUbaseMailbox(std::vector<std::uint8_t> descriptor,
                             std::uint32_t descriptor_count,
                             std::vector<std::uint8_t> context)
{
    const std::uint8_t command = descriptor[16];
    const std::uint32_t tag =
        LoadLe<std::uint32_t>(descriptor.data() + 16) >> 8;
    const std::uint32_t control =
        LoadLe<std::uint32_t>(descriptor.data() + 20);
    const std::uint16_t sequence = static_cast<std::uint16_t>(control);
    const bool event_enabled = (control & (1U << 16)) != 0;
    const auto dw = [&context](std::size_t index) {
        return LoadLe<std::uint32_t>(context.data() + index * 4);
    };

    if ((command == 0x34 || command == 0x44) && context.size() >= 64) {
        const std::uint32_t shift = dw(1) & 0x1f;
        if (shift >= 26) return RejectUbase(std::move(descriptor), descriptor_count, 22);
        const std::uint64_t iova =
            ((std::uint64_t(dw(3)) << 20) | (dw(2) >> 12)) << 12;
        if (!iova) return RejectUbase(std::move(descriptor), descriptor_count, 22);
        if (command == 0x34) {
            aeq_iova_ = iova;
            aeq_depth_ = 1U << (shift + 6);
            aeq_producer_ = 0;
            aeq_vector_ = dw(6) & 0xffffU;
        } else {
            if (tag > 255 || shift + 6 >= 24)
                return RejectUbase(std::move(descriptor), descriptor_count, 22);
            ceqs_[tag] = EventQueueContext{iova, 1U << (shift + 6), 0, dw(6) & 0xffffU};
        }
    } else if (command == 0x24 && context.size() >= 128) {
        const std::uint32_t shift = ((dw(0) >> 4) & 0xfU) + 6U;
        QueueContext queue{};
        queue.queue_iova =
            ((std::uint64_t(dw(1)) << 20) | (dw(0) >> 12)) << 12;
        queue.index_iova =
            ((std::uint64_t(dw(7) & 0x03ffffffU) << 32) | dw(6)) << 6;
        queue.depth = shift < 31 ? 1U << shift : 0;
        queue.token = dw(2) & 0x000fffffU;
        queue.moderation_count = dw(4) >> 22;
        if (!queue.moderation_count) queue.moderation_count = 1;
        queue.moderation_period = (dw(5) >> 22) & 0x7U;
        if (!queue.queue_iova || !queue.index_iova || !queue.depth)
            return RejectUbase(std::move(descriptor), descriptor_count, 22);
        jfc_contexts_[tag] = queue;
        armed_jfcs_.erase(tag);
        jfc_cqe_dma_in_flight_.erase(tag);
        std::copy_n(context.begin(), 128, jfc_query_contexts_[tag].begin());
        jfc_ceqs_[tag] = dw(2) >> 24;
    } else if (command == 0x25 && context.size() >= 256) {
        auto found = jfc_contexts_.find(tag);
        if (found == jfc_contexts_.end())
            return RejectUbase(std::move(descriptor), descriptor_count, 2);
        const std::uint32_t count_mask =
            LoadLe<std::uint32_t>(context.data() + 128 + 4 * 4);
        const std::uint32_t period_mask =
            LoadLe<std::uint32_t>(context.data() + 128 + 5 * 4);
        if ((count_mask & (0x3ffU << 22)) == 0) {
            found->second.moderation_count = dw(4) >> 22;
            if (!found->second.moderation_count)
                found->second.moderation_count = 1;
        }
        if ((period_mask & (0x7U << 22)) == 0)
            found->second.moderation_period = (dw(5) >> 22) & 0x7U;
    } else if (command == 0x54 && context.size() >= 128) {
        const std::uint32_t rqe_shift = (dw(0) >> 8) & 0xfU;
        QueueContext queue{};
        queue.queue_iova =
            ((std::uint64_t(dw(2)) << 20) | (dw(1) >> 12)) << 12;
        queue.index_iova =
            ((std::uint64_t(dw(9) & 0x000fffffU) << 32) | dw(8)) << 12;
        queue.completion_queue = ((dw(10) & 0xffU) << 12) | (dw(9) >> 20);
        queue.producer_iova =
            ((std::uint64_t(dw(12) & 3U) << 56) |
             (std::uint64_t(dw(11)) << 24) | (dw(10) >> 8)) << 6;
        queue.depth = rqe_shift < 31 ? 1U << rqe_shift : 0;
        const std::uint32_t sge_shift = (dw(0) >> 4) & 0x7U;
        queue.max_sge = 1U << sge_shift;
        queue.entry_stride = queue.max_sge * abi::kSgeBytes;
        queue.token = ((dw(1) & 0x3fU) << 14) | ((dw(0) >> 18) & 0x3fffU);
        queue.payload_token = (dw(3) >> 2) & 0x000fffffU;
        if (!queue.queue_iova || !queue.index_iova ||
            !queue.producer_iova || !queue.depth)
            return RejectUbase(std::move(descriptor), descriptor_count, 22);
        jfr_contexts_[tag] = queue;
    } else if (command == 0x04 && context.size() >= 128) {
        const std::uint32_t sq_shift = (dw(0) >> 8) & 0xfU;
        QueueContext queue{};
        queue.queue_iova =
            ((std::uint64_t(dw(2)) << 20) | (dw(1) >> 12)) << 12;
        queue.depth = sq_shift < 31 ? 1U << sq_shift : 0;
        queue.completion_queue = dw(4) & 0x000fffffU;
        queue.token = ((dw(1) & 0xffU) << 12) | ((dw(0) >> 20) & 0xfffU);
        queue.payload_token = (dw(9) >> 2) & 0x000fffffU;
        queue.receive_queue = ((dw(5) & 0xffU) << 12) | (dw(4) >> 20);
        queue.receive_completion_queue = dw(5) >> 12;
        queue.eid_index = dw(6) & 0x3ffU;
        queue.user_queue = std::uint64_t(dw(7)) | (std::uint64_t(dw(8)) << 32);
        queue.device_page_offset = abi::kUdmaMemoryOffset +
            abi::kHardwarePageBytes + std::uint64_t(tag) * abi::kHardwarePageBytes;
        queue.is_jetty = ((dw(0) >> 19) & 1U) != 0;
        if (!queue.queue_iova || !queue.depth)
            return RejectUbase(std::move(descriptor), descriptor_count, 22);
        jetty_contexts_[tag] = queue;
    }

    auto after = event_enabled ? std::function<void()>([this, sequence]() {
        EmitMailboxEvent(sequence, [this](bool ok) {
            if (!ok) ++ubase_errors_;
        });
    }) : std::function<void()>{};
    FinishUbaseDescriptor(std::move(descriptor), 0x7000, descriptor_count, {},
                          std::move(after));
}

void
UdmaModel::EmitMailboxEvent(std::uint16_t sequence, Completion completion)
{
    if (draining_eq_command_ == 0x37) {
        completion(false);
        return;
    }
    mailbox_events_.push_back({sequence, std::move(completion)});
    ProcessMailboxEvent();
}

void
UdmaModel::ProcessMailboxEvent()
{
    if (aeq_in_flight_) return;
    if (mailbox_events_.empty()) {
        MaybeCompleteEqDestroy();
        return;
    }
    auto pending = std::move(mailbox_events_.front());
    mailbox_events_.pop_front();
    const auto sequence = pending.sequence;
    auto completion = std::move(pending.completion);
    if (!aeq_iova_ || !aeq_depth_ || (aeq_depth_ & (aeq_depth_ - 1))) {
        completion(false);
        ProcessMailboxEvent();
        return;
    }
    std::vector<std::uint8_t> event(64, 0);
    std::uint32_t header = 0x13;
    if (!(aeq_producer_ & aeq_depth_)) header |= 1U << 31;
    StoreLe<std::uint32_t>(event, 0, header);
    event[12] = static_cast<std::uint8_t>(sequence);
    event[13] = static_cast<std::uint8_t>(sequence >> 8);
    const std::uint32_t slot = aeq_producer_ & (aeq_depth_ - 1);
    ++aeq_in_flight_;
    WriteIoVirtual(aeq_iova_ + std::uint64_t(slot) * 64,
                            std::move(event),
        [this, completion = std::move(completion)](bool ok) mutable {
            if (ok) {
                ++aeq_producer_;
                ubase_command_source_ |= 1U << 1;
                RaiseInterrupt(aeq_vector_,
                    [this, completion = std::move(completion)](bool interrupt_ok) mutable {
                        --aeq_in_flight_;
                        completion(interrupt_ok);
                        ProcessMailboxEvent();
                    });
                return;
            }
            --aeq_in_flight_;
            completion(false);
            ProcessMailboxEvent();
        });
}

void
UdmaModel::MaybeCompleteEqDestroy()
{
    if (!pending_eq_destroy_) return;
    if (draining_eq_command_ == 0x37 &&
        (aeq_in_flight_ || !mailbox_events_.empty())) return;
    if (draining_eq_command_ == 0x47 && CeqBusy(draining_eq_id_)) return;
    auto complete = std::move(pending_eq_destroy_);
    pending_eq_destroy_ = {};
    draining_eq_command_ = 0;
    complete();
}

std::array<std::uint8_t, 128>
UdmaModel::BuildJfcQueryContext(std::uint32_t id) const
{
    auto image = jfc_query_contexts_.at(id);
    const auto found = jfc_contexts_.find(id);
    if (found == jfc_contexts_.end()) return image;
    const auto& queue = found->second;
    // Official udma_jfc_ctx: DW4.pi, DW5.ci and DW18.wr_cqe_idx are 22 bits.
    // The serial publication engine reserves at most one DMA slot at a time.
    const auto set_bits = [&image](std::size_t word, std::uint32_t mask,
                                  std::uint32_t value) {
        const auto previous = LoadLe<std::uint32_t>(image.data() + word * 4);
        const auto next = (previous & ~mask) | (value & mask);
        for (std::size_t byte = 0; byte < 4; ++byte)
            image[word * 4 + byte] = static_cast<std::uint8_t>(next >> (byte * 8));
    };
    set_bits(4, 0x003fffffU, static_cast<std::uint32_t>(queue.producer +
        (!config_.pipelined_completion_dma && queue.cqe_write_pending)));
    set_bits(4, 0xffc00000U, queue.moderation_count << 22);
    set_bits(5, 0x003fffffU, static_cast<std::uint32_t>(queue.consumer));
    set_bits(5, 0x01c00000U, queue.moderation_period << 22);
    set_bits(18, 0x003fffffU, static_cast<std::uint32_t>(queue.producer));
    return image;
}

void
UdmaModel::QueryJfcContext(std::uint32_t id, ReadCompletion completion)
{
    if (!jfc_query_contexts_.count(id)) return completion(false, {});
    const auto found = jfc_contexts_.find(id);
    if (found == jfc_contexts_.end()) {
        // Never DMA into an already destroyed queue's record-doorbell page.
        const auto image = BuildJfcQueryContext(id);
        return completion(true, {image.begin(), image.end()});
    }
    ReadToken(found->second.token, found->second.index_iova, 4,
        [this, id, completion = std::move(completion)]
        (bool ok, std::vector<std::uint8_t> bytes) mutable {
            const auto current = jfc_contexts_.find(id);
            if (!ok || bytes.size() != 4 || current == jfc_contexts_.end())
                return completion(false, {});
            current->second.consumer = LoadLe<std::uint32_t>(bytes.data()) & 0x003fffffU;
            const auto image = BuildJfcQueryContext(id);
            completion(true, {image.begin(), image.end()});
        });
}

void
UdmaModel::MaybeCompleteJfcDestroy()
{
    if (!pending_jfc_destroy_ || JfcPublicationBusy(*draining_jfc_id_)) return;
    auto complete = std::move(pending_jfc_destroy_);
    pending_jfc_destroy_ = {};
    draining_jfc_id_.reset();
    complete();
}

bool
UdmaModel::CeqBusy(std::uint32_t id) const
{
    // CQ DMA may have reserved notification ownership without yet enqueuing
    // its CEQE. EQ destruction must also wait for that accepted work.
    for (const auto& [jfc_id, ceq_id] : jfc_ceqs_) {
        if (ceq_id != id) continue;
        const auto publication = jfc_publications_.find(jfc_id);
        if (publication != jfc_publications_.end() &&
            (publication->second->running || publication->second->in_flight ||
             !publication->second->pending.empty()))
            return true;
        const auto jfc = jfc_contexts_.find(jfc_id);
        if (jfc != jfc_contexts_.end() && jfc->second.pending_completions &&
            jfc->second.moderation_deadline_ps) return true;
    }
    return (completion_event_busy_ && active_ceq_id_ == id) ||
        std::any_of(completion_events_.begin(), completion_events_.end(),
                    [id](const auto& event) { return event.ceq_id == id; });
}

bool
UdmaModel::HandleCeqConsumerMmio(std::uint64_t offset, std::uint32_t length,
                               std::uint64_t value)
{
    // ubase_update_eq_db(): resource 1 base, eqn[7:0], type[17:16], CI[55:32].
    if (offset != abi::kUdmaMemoryOffset || length != 8 ||
        ((value >> 16) & 3U) != 2U) return false;
    const auto id = static_cast<std::uint32_t>(value & 0xffU);
    const auto found = ceqs_.find(id);
    constexpr std::uint32_t Mask = 0x00ffffffU;
    if (found == ceqs_.end() || (value & 0xff000000fffcff00ULL)) {
        ++ceq_consumer_rejects_;
        return true;
    }
    auto& ceq = found->second;
    const std::uint32_t next = (value >> 32) & Mask;
    const std::uint32_t delta = (next - ceq.consumer) & Mask;
    const std::uint32_t published = ceq.producer - ceq.consumer;
    const bool reserved = completion_event_busy_ && ceq_write_pending_ &&
                          active_ceq_id_ == id;
    if (ceq.credit_fault || delta > published + (reserved ? 1U : 0U)) {
        ++ceq_consumer_rejects_;
        return true;
    }
    ++ceq_consumer_updates_;
    if (delta > published) {
        // The guest can see the DMA payload before its acknowledgement gets
        // back to this process. Preserve, but do not spend, this credit yet.
        ceq.pending_consumer = ceq.consumer + delta;
        ceq.consumer_pending = true;
    } else {
        ceq.consumer += delta;
    }
    ProcessCompletionEvent();
    return true;
}

bool
UdmaModel::ResourceQueueBusy(std::uint8_t command, std::uint32_t id) const
{
    if (command == 0x57)
        return active_receive_jfr_ && *active_receive_jfr_ == id;
    const auto found = jetty_contexts_.find(id);
    return (found != jetty_contexts_.end() && found->second.busy) ||
        (sq_pipelines_.count(id) &&
         !sq_pipelines_.at(id)->prepared_writes.empty()) ||
        (active_receive_jetty_ && *active_receive_jetty_ == id) ||
        read_response_dma_.count(id) ||
        std::any_of(pending_rma_.begin(), pending_rma_.end(),
                    [id](const auto& item) { return item.second.jetty_id == id; });
}

void
UdmaModel::MaybeCompleteQueueDestroy()
{
    if (!pending_queue_destroy_ ||
        ResourceQueueBusy(draining_queue_command_, draining_queue_id_)) return;
    // The active WQE has drained; unfetched SQ work is abandoned by destroy.
    // This does not implement the hardware's optional flushed-WQE CQEs.
    auto complete = std::move(pending_queue_destroy_);
    pending_queue_destroy_ = {};
    draining_queue_command_ = 0;
    complete();
}

bool
UdmaModel::HandleJettyMmio(std::uint64_t offset, std::uint32_t length,
                           std::uint64_t value)
{
    if (!length || length > 8) return false;
    for (auto& [id, jetty] : jetty_contexts_) {
        if (offset >= jetty.device_page_offset &&
            offset + length <= jetty.device_page_offset + abi::kWqebbBytes) {
            if (draining_queue_command_ == 0x07 && draining_queue_id_ == id)
                return false;
            ++jetty_mmio_writes_;
            const std::uint32_t begin =
                static_cast<std::uint32_t>(offset - jetty.device_page_offset);
            for (std::uint32_t i = 0; i < length; ++i) {
                jetty.direct_wqe[begin + i] =
                    static_cast<std::uint8_t>(value >> (8 * i));
                jetty.direct_valid |= std::uint64_t{1} << (begin + i);
            }
            if (jetty.direct_valid == ~std::uint64_t{0}) {
                jetty.direct_valid = 0;
                jetty.direct_pending = true;
                const std::uint32_t low = abi::Load32(jetty.direct_wqe.data()) & 0xffffU;
                const auto pipeline = sq_pipelines_.find(id);
                const auto issue = pipeline == sq_pipelines_.end() ? jetty.consumer :
                    pipeline->second->issue;
                std::uint32_t producer =
                    (static_cast<std::uint32_t>(issue) & ~0xffffU) | low;
                if (producer < static_cast<std::uint32_t>(issue)) producer += 0x10000U;
                ProcessSq(id, producer,
                    std::vector<std::uint8_t>(jetty.direct_wqe.begin(),
                                              jetty.direct_wqe.end()));
            }
            return true;
        }
        if (offset == jetty.device_page_offset + abi::kDoorbellOffset &&
            length == 4) {
            if (draining_queue_command_ == 0x07 && draining_queue_id_ == id)
                return false;
            ++jetty_mmio_writes_;
            ++sq_doorbells_;
            ProcessSq(id, static_cast<std::uint32_t>(value));
            return true;
        }
    }
    return false;
}

} // namespace ubsim::device
