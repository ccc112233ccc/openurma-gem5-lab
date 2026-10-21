// SPDX-License-Identifier: Apache-2.0
#include "udma_model_internal.h"

namespace ubsim::device {
using namespace detail;

void
UdmaModel::WriteCqe(std::uint32_t jfc_id, bool receive, bool jetty,
                    std::uint8_t opcode, std::uint16_t entry_index,
                    std::uint32_t local_id, std::uint32_t byte_count,
                    std::uint64_t user_data, std::uint64_t immediate,
                    Completion completion, std::uint32_t remote_id,
                    std::uint32_t remote_eid, std::uint32_t tpn,
                    std::uint8_t status, std::uint8_t substatus)
{
    // Producer ownership spans the consumer read, CQE DMA and notification.
    // Different Jettys sharing one JFC must not choose the same producer slot.
    if (draining_jfc_id_ && *draining_jfc_id_ == jfc_id)
        return completion(false);
    const auto ceq = jfc_ceqs_.find(jfc_id);
    if (draining_eq_command_ == 0x47 && ceq != jfc_ceqs_.end() &&
        ceq->second == draining_eq_id_) return completion(false);
    auto& slot = jfc_publications_[jfc_id];
    if (!slot) slot = std::make_shared<JfcPublicationQueue>();
    const auto queued_at = now_ps_;
    cqe_wait_timing_.Begin();
    slot->pending.emplace_back(
        [this, jfc_id, receive, jetty, opcode, entry_index, local_id,
         byte_count, user_data, immediate, remote_id, remote_eid, tpn, status, substatus,
         queued_at,
         completion = std::move(completion)](Completion released) mutable {
            cqe_wait_timing_.End(queued_at, now_ps_);
            const auto publishing_at = now_ps_;
            cqe_publish_timing_.Begin();
            WriteCqeNow(jfc_id, receive, jetty, opcode, entry_index,
                local_id, byte_count, user_data, immediate,
                [this, publishing_at, completion = std::move(completion),
                 released = std::move(released)](bool ok) mutable {
                    cqe_publish_timing_.End(publishing_at, now_ps_);
                    // Remain busy through the caller callback: it may submit
                    // another WQE or request destruction reentrantly.
                    completion(ok);
                    released(ok);
                }, remote_id, remote_eid, tpn, status, substatus);
        });
    jfc_pending_peak_ = std::max<std::uint64_t>(jfc_pending_peak_, slot->pending.size());
    PumpCompletionWork();
}

bool
UdmaModel::JfcPublicationBusy(std::uint32_t jfc_id) const
{
    const auto jfc = jfc_contexts_.find(jfc_id);
    if (jfc != jfc_contexts_.end() && jfc->second.pending_completions &&
        jfc->second.moderation_deadline_ps) return true;
    if ((completion_event_busy_ && active_event_jfc_id_ == jfc_id) ||
        std::any_of(completion_events_.begin(), completion_events_.end(),
                    [jfc_id](const auto& event) { return event.jfc_id == jfc_id; }))
        return true;
    const auto found = jfc_publications_.find(jfc_id);
    return found != jfc_publications_.end() &&
        (found->second->running || found->second->in_flight ||
         !found->second->pending.empty());
}

void
UdmaModel::ProcessJfcPublications(std::uint32_t jfc_id)
{
    const auto found = jfc_publications_.find(jfc_id);
    if (found == jfc_publications_.end()) return;
    const auto state = found->second;
    if (state->dispatching ||
        (!config_.pipelined_completion_dma && state->running)) return;
    state->dispatching = true;
    // The shared reservation pool bounds CQ DMA and notification work. The
    // legacy mode admits one at a time; the modern mode pipelines distinct
    // ring slots while preserving admission order.
    while ((!state->running || config_.pipelined_completion_dma) &&
           !state->pending.empty() &&
           completion_event_slots_in_use_ < config_.completion_event_slots) {
        if (config_.pipelined_completion_dma) {
            const auto jfc = jfc_contexts_.find(jfc_id);
            if (jfc == jfc_contexts_.end()) break;
            const auto used = (static_cast<std::uint32_t>(jfc->second.producer) -
                static_cast<std::uint32_t>(jfc->second.consumer)) & 0x3fffffU;
            // A near-full CQ needs a fresh consumer-index DMA read. Admit
            // that refresh only after earlier publications have reserved
            // their slots, so asynchronous read callbacks cannot reorder CQEs.
            if (used >= jfc->second.depth && state->in_flight) break;
        }
        auto issue = std::move(state->pending.front());
        state->pending.pop_front();
        if (config_.pipelined_completion_dma) ++state->in_flight;
        else state->running = true;
        ++completion_event_slots_in_use_;
        completion_slots_peak_ = std::max<std::uint64_t>(completion_slots_peak_, completion_event_slots_in_use_);
        last_jfc_admitted_ = jfc_id;
        issue([this, state](bool) {
            if (config_.pipelined_completion_dma) --state->in_flight;
            else state->running = false;
            PumpCompletionWork();
        });
        if (!config_.pipelined_completion_dma) break;
    }
    state->dispatching = false;
    if (!state->running && !state->in_flight && state->pending.empty()) {
        jfc_publications_.erase(jfc_id);
        MaybeCompleteJfcDestroy();
        // Failed CQ DMA (or a CQ not requiring an event) has no CEQ callback
        // to wake a destroy command that waited for this publication.
        MaybeCompleteEqDestroy();
    }
}

void
UdmaModel::ReleaseCompletionEventSlot()
{
    --completion_event_slots_in_use_;
    PumpCompletionWork();
}

void
UdmaModel::PumpCompletionWork()
{
    completion_work_kick_ = true;
    if (completion_work_dispatching_) return;
    completion_work_dispatching_ = true;
    do {
        completion_work_kick_ = false;
        // Expired moderation timers get admission before further CQ work.
        // Snapshot IDs: synchronous callbacks may destroy their contexts.
        std::vector<std::uint32_t> expired;
        for (const auto& [id, jfc] : jfc_contexts_)
            if (jfc.pending_completions && jfc.moderation_deadline_ps &&
                jfc.moderation_deadline_ps <= now_ps_) expired.push_back(id);
        for (const auto id : expired) {
            if (completion_event_slots_in_use_ >= config_.completion_event_slots) break;
            const auto found = jfc_contexts_.find(id);
            if (found == jfc_contexts_.end()) continue;
            found->second.pending_completions = 0;
            found->second.moderation_deadline_ps = 0;
            ++completion_event_slots_in_use_;
            EmitCompletionEvent(id, [this](bool ok) {
                if (!ok) { ++ubase_errors_; ++notification_failures_; }
                ReleaseCompletionEventSlot();
            });
        }
        std::vector<std::uint32_t> ids;
        for (const auto& [id, state] : jfc_publications_) {
            (void)state;
            ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());
        if (last_jfc_admitted_)
            std::rotate(ids.begin(),
                std::upper_bound(ids.begin(), ids.end(), *last_jfc_admitted_), ids.end());
        for (const auto id : ids) ProcessJfcPublications(id);
    } while (completion_work_kick_);
    completion_work_dispatching_ = false;
}

void
UdmaModel::WriteCqeNow(std::uint32_t jfc_id, bool receive, bool jetty,
                    std::uint8_t opcode, std::uint16_t entry_index,
                    std::uint32_t local_id, std::uint32_t byte_count,
                    std::uint64_t user_data, std::uint64_t immediate,
                    Completion completion, std::uint32_t remote_id,
                    std::uint32_t remote_eid, std::uint32_t tpn,
                    std::uint8_t status, std::uint8_t substatus)
{
    // The caller reserved one notification slot before beginning CQ DMA.
    // Keep that reservation until either no event is needed, or its MSI ends.
    Completion finish = [this, completion](bool ok) {
        completion(ok);
        ReleaseCompletionEventSlot();
    };
    auto found = jfc_contexts_.find(jfc_id);
    if (found == jfc_contexts_.end()) return finish(false);
    const std::uint64_t ci = found->second.index_iova;
    const std::uint32_t token = found->second.token;
    auto publish = [this, jfc_id, receive, jetty, opcode, entry_index, local_id,
         byte_count, user_data, immediate, remote_id, remote_eid, tpn, status, substatus,
         completion = std::move(completion), finish = std::move(finish)]
        // remote identity is captured separately because it is produced by
        // the peer packet rather than the local queue context.
        (bool ok, std::vector<std::uint8_t> consumer_bytes) mutable {
            auto found = jfc_contexts_.find(jfc_id);
            if (!ok || consumer_bytes.size() != 4 ||
                found == jfc_contexts_.end()) return finish(false);
            QueueContext& jfc = found->second;
            const std::uint32_t consumer = abi::Load32(consumer_bytes.data()) & 0x3fffffU;
            const std::uint32_t producer = static_cast<std::uint32_t>(jfc.producer) & 0x3fffffU;
            if (((producer - consumer) & 0x3fffffU) >= jfc.depth)
                return finish(false);
            const bool owner = ((jfc.producer / jfc.depth) & 1U) == 0;
            const auto cqe = abi::MakeCqe(receive, jetty, owner, opcode,
                entry_index, local_id, byte_count, user_data, immediate,
                remote_id, remote_eid, tpn, status, substatus);
            const std::uint64_t address = jfc.queue_iova +
                (jfc.producer & (jfc.depth - 1U)) * abi::kCqeBytes;
            jfc.consumer = consumer;
            jfc.cqe_write_pending = true;
            if (config_.pipelined_completion_dma) {
                // Reserve a unique CQ slot before an asynchronous DMA. Later
                // slots may finish first; the polling consumer observes the
                // owner bit only after the earlier slot becomes visible.
                ++jfc.producer;
                ++jfc_cqe_dma_in_flight_[jfc_id];
            }
            WriteToken(jfc.token, address,
                std::vector<std::uint8_t>(cqe.begin(), cqe.end()),
                [this, jfc_id, completion = std::move(completion),
                 finish = std::move(finish)](bool write_ok) mutable {
                    auto found = jfc_contexts_.find(jfc_id);
                    if (found != jfc_contexts_.end()) {
                        if (config_.pipelined_completion_dma) {
                            auto active = jfc_cqe_dma_in_flight_.find(jfc_id);
                            if (active == jfc_cqe_dma_in_flight_.end() ||
                                !active->second) return finish(false);
                            --active->second;
                            found->second.cqe_write_pending = active->second != 0;
                            if (!active->second)
                                jfc_cqe_dma_in_flight_.erase(active);
                        } else found->second.cqe_write_pending = false;
                    }
                    if (!write_ok || found == jfc_contexts_.end())
                        return finish(false);
                    if (!config_.pipelined_completion_dma)
                        ++found->second.producer;
                    ++found->second.pending_completions;
                    static constexpr std::array<std::uint64_t, 8> PeriodUs{
                        0, 4, 16, 64, 256, 1024, 4096, 16384};
                    if (found->second.pending_completions <
                        found->second.moderation_count) {
                        if (!found->second.moderation_deadline_ps &&
                            found->second.moderation_period) {
                            found->second.moderation_deadline_ps = now_ps_ +
                                PeriodUs[found->second.moderation_period] *
                                    1000000ULL;
                        }
                        finish(true);
                        return;
                    }
                    found->second.pending_completions = 0;
                    found->second.moderation_deadline_ps = 0;
                    // The CQE is already visible and its producer committed.
                    // Register notification ownership BEFORE releasing CQ/SQ:
                    // a reentrant completion can request JFC destruction.
                    // Failure of the independent notification must never
                    // replay this WQE or undo an already visible CQE.
                    EmitCompletionEvent(jfc_id,
                        [this](bool event_ok) {
                            if (!event_ok) { ++ubase_errors_; ++notification_failures_; }
                            ReleaseCompletionEventSlot();
                        });
                    completion(true);
                });
        };
    // The cached consumer is a conservative lower bound. As long as the
    // reserved producer remains inside the ring, another host DMA read of
    // the record doorbell cannot add safety; refresh only near a full JFC.
    const auto& jfc = found->second;
    const auto used = (static_cast<std::uint32_t>(jfc.producer) -
                       static_cast<std::uint32_t>(jfc.consumer)) & 0x3fffffU;
    if (config_.pipelined_completion_dma && used < jfc.depth) {
        std::vector<std::uint8_t> cached(4);
        StoreLe<std::uint32_t>(cached, 0,
            static_cast<std::uint32_t>(jfc.consumer) & 0x3fffffU);
        publish(true, std::move(cached));
    } else {
        ReadToken(token, ci, 4, std::move(publish));
    }
}

void
UdmaModel::EmitCompletionEvent(std::uint32_t jfc_id, Completion completion)
{
    // Reservations were acquired by accepted CQ work or its moderation timer.
    // They still drain after destroy is requested; WriteCqe gates NEW work.
    if (config_.one_shot_jfc_notifications) {
        auto armed = armed_jfcs_.find(jfc_id);
        if (armed == armed_jfcs_.end() || !armed->second) {
            completion(true);
            return;
        }
        armed->second = 0;
    }
    const auto mapping = jfc_ceqs_.find(jfc_id);
    if (mapping == jfc_ceqs_.end()) {
        completion(false);
        return;
    }
    const auto ceq_id = mapping->second;
    completion_events_.push_back({jfc_id, ceq_id, std::move(completion)});
    event_queue_peak_ = std::max<std::uint64_t>(event_queue_peak_,
        completion_events_.size() + (completion_event_busy_ ? 1U : 0U));
    ProcessCompletionEvent();
}

void
UdmaModel::ProcessCompletionEvent()
{
    if (completion_event_busy_ || completion_event_dispatching_) return;
    completion_event_dispatching_ = true;
    while (!completion_event_busy_ && !completion_events_.empty()) {
        // A full CEQ must not head-of-line block another CEQ with credits.
        // The first eligible event preserves ordering within each CEQ.
        const auto next = std::find_if(completion_events_.begin(),
            completion_events_.end(), [this](const auto& event) {
                const auto found = ceqs_.find(event.ceq_id);
                if (found == ceqs_.end()) return true;
                const auto& ceq = found->second;
                return !ceq.credit_fault && ceq.producer - ceq.consumer < ceq.depth;
            });
        if (next == completion_events_.end()) {
            ++ceq_full_waits_;
            break;
        }
        PendingCompletionEvent pending = std::move(*next);
        completion_events_.erase(next);
        const auto found = ceqs_.find(pending.ceq_id);
        if (found == ceqs_.end()) {
            pending.completion(false);
            continue;
        }
        completion_event_busy_ = true;
        ceq_write_pending_ = true;
        active_ceq_id_ = pending.ceq_id;
        active_event_jfc_id_ = pending.jfc_id;
        auto& ceq = found->second;
        std::vector<std::uint8_t> event(64, 0);
        std::uint32_t word = pending.jfc_id & 0xfffffU;
        if (!(ceq.producer & ceq.depth)) word |= 1U << 31;
        StoreLe<std::uint32_t>(event, 0, word);
        const std::uint32_t slot = ceq.producer & (ceq.depth - 1);
        WriteIoVirtual(ceq.iova + std::uint64_t(slot) * 64,
                       std::move(event),
            [this, ceq_id = pending.ceq_id, vector = ceq.vector,
             completion = std::move(pending.completion)](bool ok) mutable {
                ceq_write_pending_ = false;
                auto& ceq = ceqs_.at(ceq_id);
                if (ok) {
                    ++ceq.producer;
                    ++events_published_;
                    if (ceq.consumer_pending) {
                        ceq.consumer = ceq.pending_consumer;
                        ceq.consumer_pending = false;
                    }
                    RaiseInterrupt(vector,
                        [this, completion = std::move(completion)]
                        (bool interrupt_ok) mutable {
                            completion_event_busy_ = false;
                            completion(interrupt_ok);
                            MaybeCompleteEqDestroy();
                            MaybeCompleteJfcDestroy();
                            ProcessCompletionEvent();
                        });
                    return;
                }
                if (ceq.consumer_pending) {
                    // A consumption report combined with failed DMA has
                    // ambiguous visibility. Do not reuse this ring slot or
                    // checkpoint it as a clean state.
                    ceq.consumer_pending = false;
                    ceq.credit_fault = true;
                    ++ceq_consumer_rejects_;
                }
                completion_event_busy_ = false;
                completion(false);
                MaybeCompleteEqDestroy();
                MaybeCompleteJfcDestroy();
                ProcessCompletionEvent();
            });
    }
    completion_event_dispatching_ = false;
    MaybeCompleteEqDestroy();
    MaybeCompleteJfcDestroy();
}

void
UdmaModel::RaiseInterrupt(std::uint32_t vector, Completion completion)
{
    constexpr std::uint32_t Enable = 0x40c04;
    constexpr std::uint32_t EnabledVectors = 0x40c0c;
    constexpr std::uint32_t Data = 0x40c10;
    constexpr std::uint32_t AddressLow = 0x40c14;
    constexpr std::uint32_t AddressHigh = 0x40c18;
    constexpr std::uint32_t Mask = 0x40c20;
    const auto reg = [this](std::uint32_t address) {
        const auto found = ubios_endpoint_config_.find(address);
        return found == ubios_endpoint_config_.end() ? 0U : found->second;
    };
    const std::uint32_t enabled_log2 = reg(EnabledVectors);
    const std::uint32_t count = enabled_log2 < 31 ? 1U << enabled_log2 : 0;
    const std::uint64_t address = reg(AddressLow) |
        (std::uint64_t(reg(AddressHigh)) << 32);
    if (!(reg(Enable) & 1U) || !address) {
        // Pre-MSI bootstrap compatibility: the host adapter exposes the same
        // logical vector until the official UBUS code programs Type-1 MSI.
        host_.PulseInterrupt(vector);
        completion(true);
        return;
    }
    if (vector >= count || (vector < 32 && (reg(Mask) & (1U << vector))))
        return completion(false);
    auto state = std::make_shared<MsiState>();
    state->vector = vector;
    state->address = address;
    state->data = reg(Data) + vector;
    state->completion = std::move(completion);
    if (msi_iova_ == address && msi_physical_) {
        host_.MsiWrite(msi_physical_, state->data,
                       std::move(state->completion));
        return;
    }
    // The administrative UBASE MSI mapping exists before any userspace UDMA
    // queue context is created, so its token cannot be inferred from JFC/JFR
    // or jetty state.  Resolve it through the UMMU TCT just like every other
    // device-visible IOVA.  This also avoids assuming that the kernel chose a
    // particular token number.
    TranslateIoVirtual(state->address, true,
        [this, state = std::move(state)]
        (bool ok, std::uint64_t physical) mutable {
            if (!ok && config_.identity_iova_test_mode) {
                ok = true;
                physical = state->address;
            }
            if (!ok || !physical) {
                std::cerr << "udma-model: MSI IOVA translation failed address=0x"
                          << std::hex << state->address << std::dec << '\n';
                return state->completion(false);
            }
            msi_iova_ = state->address;
            msi_physical_ = physical;
            host_.MsiWrite(physical, state->data,
                           std::move(state->completion));
        });
}

} // namespace ubsim::device
