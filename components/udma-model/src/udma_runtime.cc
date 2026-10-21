// SPDX-License-Identifier: Apache-2.0
#include "udma_model_internal.h"

namespace ubsim::device {
using namespace detail;

void
UdmaModel::FetchDescriptor(std::uint64_t address)
{
    host_.DmaRead(address, sizeof(Descriptor),
        [this](bool ok, std::vector<std::uint8_t> bytes) {
            if (!ok || bytes.size() != sizeof(Descriptor)) {
                return;
            }
            Descriptor descriptor{};
            std::memcpy(&descriptor, bytes.data(), sizeof(descriptor));
            const std::uint64_t sequence = next_sequence_++;
            ++submitted_;
            if (descriptor.opcode != kOpcodeSend || descriptor.length == 0) {
                Finish(sequence, descriptor, kStatusDmaError);
                return;
            }
            pending_.emplace(sequence, descriptor);
            FetchPayload(sequence, descriptor);
        });
}

void
UdmaModel::FetchPayload(std::uint64_t sequence, Descriptor descriptor)
{
    host_.DmaRead(descriptor.payload_address, descriptor.length,
        [this, sequence, descriptor](bool ok, std::vector<std::uint8_t> bytes) mutable {
            if (!ok || bytes.size() != descriptor.length) {
                Finish(sequence, descriptor, kStatusDmaError);
                return;
            }
            Frame frame{};
            frame.sequence = sequence;
            frame.source_eid = descriptor.source_eid;
            frame.destination_eid = descriptor.destination_eid;
            frame.source_port = descriptor.source_port;
            frame.destination_port = descriptor.source_port;
            frame.bytes = std::move(bytes);
            network_.Send(std::move(frame),
                [this, sequence, descriptor](bool sent) {
                    Finish(sequence, descriptor,
                           sent ? kStatusSuccess : kStatusDmaError);
                });
        });
}

void
UdmaModel::Finish(std::uint64_t sequence, const Descriptor& descriptor,
                  std::uint32_t status)
{
    CompletionEntry entry{sequence, status, descriptor.length};
    const auto* first = reinterpret_cast<const std::uint8_t*>(&entry);
    host_.DmaWrite(descriptor.completion_address,
                   std::vector<std::uint8_t>(first, first + sizeof(entry)),
        [this, sequence](bool ok) {
            pending_.erase(sequence);
            if (!ok) {
                return;
            }
            ++completed_;
            host_.SetInterrupt(kInterruptVector, true);
        });
}

void
UdmaModel::Receive(Frame frame)
{
    switch (frame.operation) {
      case Frame::Operation::Send:
      case Frame::Operation::SendImmediate:
        receive_frames_.push_back(std::move(frame));
        ProcessReceiveQueue();
        return;
      case Frame::Operation::Write: return ReceiveWrite(std::move(frame));
      case Frame::Operation::ReadRequest:
        return ReceiveReadRequest(std::move(frame));
      case Frame::Operation::WriteAck: return ReceiveWriteAck(std::move(frame));
      case Frame::Operation::ReadResponse:
        return ReceiveReadResponse(std::move(frame));
      case Frame::Operation::RmaError: {
        auto found = pending_rma_.find(frame.request_id);
        if (found == pending_rma_.end()) { ++ubase_errors_; return; }
        found->second.failed = true;
        // An error completion permits the guest to recycle the destination
        // buffer. Do not publish it before previously accepted DMA has drained.
        if (found->second.response_dma) return;
        const PendingRma pending = found->second;
        pending_rma_.erase(found);
        CompleteSq(pending.jetty_id, pending.producer, pending.wqebbs,
                   pending.completed_index, pending.opcode, pending.byte_count,
                   pending.immediate, pending.completion, 5);
        return;
      }
      default: ++ubase_errors_; return;
    }
}

void
UdmaModel::SetLinkState(std::uint32_t port, bool up)
{
    if (port >= link_up_.size()) { ++ubase_errors_; return; }
    link_up_[port] = up;
    if (up) return;
    for (auto& [id, route] : tp_routes_) {
        (void)id;
        if (route.port != port) continue;
        bool rebound = false;
        for (std::uint32_t candidate = 0; candidate < link_up_.size(); ++candidate) {
            if (link_up_[candidate]) {
                route.port = candidate;
                rebound = true;
                break;
            }
        }
        if (!rebound) route.active = false;
    }
}

void
UdmaModel::AdvanceTime(std::uint64_t now_ps)
{
    if (now_ps < now_ps_) return;
    now_ps_ = now_ps;
    PumpCompletionWork();
}

std::uint64_t
UdmaModel::NextEventTime() const
{
    std::uint64_t next = std::numeric_limits<std::uint64_t>::max();
    // A full reservation pool needs an external completion/consumer update,
    // not repeated zero-time polling of an already expired timer.
    if (completion_event_slots_in_use_ >= config_.completion_event_slots) return next;
    for (const auto& [jfc_id, jfc] : jfc_contexts_) {
        (void)jfc_id;
        if (jfc.pending_completions && jfc.moderation_deadline_ps)
            next = std::min(next, jfc.moderation_deadline_ps);
    }
    return next;
}

void
UdmaModel::DumpProgress(std::ostream& output) const
{
    output << "[UDMA_PROGRESS] now_ps=" << now_ps_
           << " sq_wqes=" << sq_wqes_
           << " sq_completions=" << sq_completions_
           << " quiescent=" << IsQuiescent()
           << " pending_rma=" << pending_rma_.size()
           << " read_dma_queues=" << read_response_dma_.size()
           << " remote_services=" << remote_services_in_flight_
           << " completion_events=" << completion_events_.size()
           << " event_busy=" << completion_event_busy_
           << " receive_busy=" << receive_busy_
           << " event_slots_in_use=" << completion_event_slots_in_use_
           << " destroy_pending=" << bool(pending_queue_destroy_) << '\n';
    for (const auto& [id, state] : sq_pipelines_) {
        const auto queue = jetty_contexts_.find(id);
        if (queue == jetty_contexts_.end()) continue;
        if (!state->fetching && !state->prefetch_inflight && !state->prefetch_failed && state->completions.empty() &&
            state->issue == queue->second.target_producer) continue;
        output << "[UDMA_SQ_PROGRESS] id=" << id
               << " depth=" << queue->second.depth
               << " target=" << queue->second.target_producer
               << " issue=" << state->issue
               << " retired=" << queue->second.consumer
               << " fetching=" << state->fetching
               << " prefetch_inflight=" << state->prefetch_inflight
               << " prefetch_failed=" << state->prefetch_failed
               << " cached_wqebbs=" << state->cache.size()
               << " publishing=" << state->publishing
               << " ordered=" << state->ordered
               << " entries=" << state->completions.size()
               << " ready=" << std::count_if(state->completions.begin(),
                    state->completions.end(), [](const auto& c) { return c.ready; })
               << " head_ready=" << (!state->completions.empty() && state->completions.front().ready)
               << " jfc=" << queue->second.completion_queue << '\n';
    }
    for (const auto& [id, state] : jfc_publications_) {
        if (!state->running && !state->in_flight && state->pending.empty()) continue;
        const auto queue = jfc_contexts_.find(id);
        output << "[UDMA_JFC_PROGRESS] id=" << id
               << " running=" << state->running
               << " in_flight=" << state->in_flight
               << " queued=" << state->pending.size();
        if (queue != jfc_contexts_.end())
            output << " producer=" << queue->second.producer
                   << " last_consumer=" << queue->second.consumer
                   << " cqe_dma=" << queue->second.cqe_write_pending;
        output << '\n';
    }
}

void
UdmaModel::DumpWriteTiming(std::ostream& output) const
{
    const auto stage = [&output](const char* name, const TimingStage& value) {
        output << "[UDMA_WRITE_STAGE] name=" << name
               << " count=" << value.count
               << " total_ps=" << value.total_ps
               << " max_ps=" << value.max_ps
               << " peak_in_flight=" << value.peak_in_flight
               << " remaining=" << value.in_flight << '\n';
    };
    stage("sq_prefetch", sq_prefetch_timing_);
    stage("source_payload", source_payload_timing_);
    stage("target_write", target_write_timing_);
    stage("write_ack", write_ack_timing_);
    stage("cqe_wait", cqe_wait_timing_);
    stage("cqe_publish", cqe_publish_timing_);
    output << "[UDMA_WRITE_RESOURCE] completion_slots_peak="
           << completion_slots_peak_
           << " completion_slots_limit=" << config_.completion_event_slots
           << " jfc_pending_peak=" << jfc_pending_peak_
           << " pending_ack_starts=" << write_ack_starts_.size() << '\n';
}

bool
UdmaModel::IsQuiescent() const
{
    if (completion_event_slots_in_use_) return false;
    for (const auto& [queue, busy] : ummu_command_queue_busy_) {
        (void)queue;
        if (busy) return false;
    }
    for (const auto& [id, ceq] : ceqs_) {
        (void)id;
        if (ceq.consumer_pending || ceq.credit_fault) return false;
    }
    for (const auto& entry : sq_pipelines_)
        if (entry.second->prefetch_inflight || entry.second->prefetch_failed ||
            !entry.second->cache.empty() || !entry.second->prepared_writes.empty())
            return false;
    if (ubios_busy_ || ubase_busy_ || pending_queue_destroy_ ||
        remote_services_in_flight_ ||
        pending_jfc_destroy_ || pending_eq_destroy_ ||
        active_receive_jfr_ || active_receive_jetty_ || !read_response_dma_.empty() ||
        aeq_in_flight_ || !mailbox_events_.empty() ||
        !jfc_publications_.empty() || completion_event_busy_ ||
        !completion_events_.empty() || !pending_.empty() || receive_busy_ ||
        !receive_frames_.empty() || !pending_rma_.empty())
        return false;
    const auto queue_busy = [](const auto& contexts) {
        return std::any_of(contexts.begin(), contexts.end(),
            [](const auto& entry) {
                const auto& queue = entry.second;
                return queue.busy || queue.direct_pending ||
                       queue.pending_completions != 0;
            });
    };
    return !queue_busy(jfc_contexts_) && !queue_busy(jfr_contexts_) &&
           !queue_busy(jetty_contexts_);
}

void
UdmaModel::RebaseTime(std::uint64_t now_ps)
{
    if (!IsQuiescent())
        return;
    now_ps_ = now_ps;
    for (auto& [id, jfc] : jfc_contexts_) {
        (void)id;
        jfc.moderation_deadline_ps = 0;
    }
}

bool
UdmaModel::SaveState(std::ostream& output) const
{
    if (!IsQuiescent()) return false;
    const std::uint32_t version = 4;
    const std::uint8_t has_token = generic_iova_token_.has_value();
    const std::uint32_t token = generic_iova_token_.value_or(0);
    if (!WritePod(output, kStateMagic) || !WritePod(output, version) ||
        !WritePod(output, config_.mmio_base) ||
        !WritePod(output, config_.port_count) ||
        !WritePod(output, config_.endpoint_eid) ||
        !WritePod(output, ummu_registers_) || !WritePod(output, has_token) ||
        !WritePod(output, token) || !WritePod(output, ubios_message_queue_registers_) ||
        !WritePod(output, ubase_command_queue_registers_) ||
        !WritePod(output, ubase_command_source_) ||
        !WritePodMap(output, ubios_root_config_) ||
        !WritePodMap(output, ubios_endpoint_config_) ||
        !WritePod(output, ubios_root_cna_) ||
        !WritePod(output, ubios_endpoint_cna_) ||
        !WritePod(output, ubios_target_producer_) ||
        !WritePod(output, ubios_errors_) ||
        !WritePod(output, ubase_target_producer_) ||
        !WritePod(output, ubase_errors_) ||
        !WritePod(output, msi_iova_) || !WritePod(output, msi_physical_) ||
        !WritePod(output, aeq_iova_) || !WritePod(output, aeq_depth_) ||
        !WritePod(output, aeq_producer_) || !WritePod(output, aeq_vector_) ||
        !WritePodMap(output, ceqs_) ||
        !WritePodMap(output, jfc_ceqs_) ||
        !WritePodMap(output, jfc_contexts_) ||
        !WritePodMap(output, jfc_query_contexts_) ||
        !WritePodMap(output, jfr_contexts_) ||
        !WritePodMap(output, jetty_contexts_) ||
        !WritePodMap(output, tp_routes_) || !WritePod(output, next_tp_id_) ||
        !WritePod(output, next_tp_port_) || !WritePod(output, next_sequence_) ||
        !WritePod(output, next_rma_request_) || !WritePod(output, submitted_) ||
        !WritePod(output, completed_))
        return false;
    const std::uint64_t links = link_up_.size();
    if (!WritePod(output, links)) return false;
    for (bool up : link_up_) {
        const std::uint8_t value = up;
        if (!WritePod(output, value)) return false;
    }
    if (!WritePodMap(output, armed_jfcs_)) return false;
    return output.good();
}

bool
UdmaModel::LoadState(std::istream& input)
{
    // Outstanding callbacks belong to the current state and cannot be
    // cancelled by replacing the serialized queue contents.
    if (!IsQuiescent()) return false;
    std::uint64_t magic{};
    std::uint32_t version{};
    std::uint64_t mmio_base{};
    std::uint32_t port_count{};
    std::uint32_t endpoint_eid{};
    std::uint8_t has_token{};
    std::uint32_t token{};
    if (!ReadPod(input, magic) || magic != kStateMagic ||
        !ReadPod(input, version) || (version != 3 && version != 4) ||
        !ReadPod(input, mmio_base) || mmio_base != config_.mmio_base ||
        !ReadPod(input, port_count) || port_count != config_.port_count ||
        !ReadPod(input, endpoint_eid) || endpoint_eid != config_.endpoint_eid ||
        !ReadPod(input, ummu_registers_) || !ReadPod(input, has_token) ||
        !ReadPod(input, token) || !ReadPod(input, ubios_message_queue_registers_) ||
        !ReadPod(input, ubase_command_queue_registers_) ||
        !ReadPod(input, ubase_command_source_) ||
        !ReadPodMap(input, ubios_root_config_) ||
        !ReadPodMap(input, ubios_endpoint_config_) ||
        !ReadPod(input, ubios_root_cna_) ||
        !ReadPod(input, ubios_endpoint_cna_) ||
        !ReadPod(input, ubios_target_producer_) || !ReadPod(input, ubios_errors_) ||
        !ReadPod(input, ubase_target_producer_) || !ReadPod(input, ubase_errors_) ||
        !ReadPod(input, msi_iova_) || !ReadPod(input, msi_physical_) ||
        !ReadPod(input, aeq_iova_) || !ReadPod(input, aeq_depth_) ||
        !ReadPod(input, aeq_producer_) || !ReadPod(input, aeq_vector_) ||
        !ReadPodMap(input, ceqs_) ||
        !ReadPodMap(input, jfc_ceqs_) ||
        !ReadPodMap(input, jfc_contexts_) || !ReadPodMap(input, jfc_query_contexts_) ||
        !ReadPodMap(input, jfr_contexts_) ||
        !ReadPodMap(input, jetty_contexts_) || !ReadPodMap(input, tp_routes_) ||
        !ReadPod(input, next_tp_id_) || !ReadPod(input, next_tp_port_) ||
        !ReadPod(input, next_sequence_) || !ReadPod(input, next_rma_request_) ||
        !ReadPod(input, submitted_) || !ReadPod(input, completed_))
        return false;
    generic_iova_token_ = has_token ? std::optional<std::uint32_t>(token)
                                    : std::nullopt;
    // Translation caches are microarchitectural state and are deliberately
    // refilled after restore rather than serialized in checkpoints.
    iotlb_.clear();
    ummu_command_queue_busy_.clear();
    std::uint64_t links{};
    if (!ReadPod(input, links) || links != config_.port_count) return false;
    link_up_.assign(links, false);
    for (std::uint64_t i = 0; i < links; ++i) {
        std::uint8_t up{};
        if (!ReadPod(input, up)) return false;
        link_up_[i] = up != 0;
    }
    armed_jfcs_.clear();
    if (version == 4 && !ReadPodMap(input, armed_jfcs_)) return false;
    ubios_busy_ = ubase_busy_ = completion_event_busy_ = receive_busy_ = false;
    ceq_write_pending_ = completion_event_dispatching_ = false;
    completion_work_dispatching_ = completion_work_kick_ = false;
    completion_event_slots_in_use_ = 0;
    last_jfc_admitted_.reset();
    completion_events_.clear();
    mailbox_events_.clear();
    jfc_publications_.clear();
    jfc_cqe_dma_in_flight_.clear();
    // Quiescent SQ issue cursors equal the restored retired consumers.
    // Reconstruct lazily; never retain runtime cursors from an earlier image.
    sq_pipelines_.clear();
    aeq_in_flight_ = 0;
    draining_eq_command_ = draining_queue_command_ = 0;
    pending_eq_destroy_ = pending_jfc_destroy_ = pending_queue_destroy_ = {};
    draining_jfc_id_.reset();
    active_receive_jfr_.reset();
    active_receive_jetty_.reset();
    read_response_dma_.clear();
    remote_services_in_flight_ = 0;
    pending_.clear();
    receive_frames_.clear();
    pending_rma_.clear();
    now_ps_ = 0;
    return input.good();
}

} // namespace ubsim::device
