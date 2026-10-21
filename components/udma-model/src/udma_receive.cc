// SPDX-License-Identifier: Apache-2.0
#include "udma_model_internal.h"

namespace ubsim::device {
using namespace detail;

void
UdmaModel::ReceiveWrite(Frame frame)
{
    if (!frame.remote_address || !frame.transfer_length ||
        frame.transfer_length - 1 > UINT64_MAX - frame.remote_address ||
        frame.bytes.size() != frame.transfer_length) {
        ++ubase_errors_;
        return;
    }
    const Frame reply_basis = frame;
    ++remote_services_in_flight_;
    const auto target_write_start = now_ps_;
    target_write_timing_.Begin();
    WriteToken(frame.segment, frame.remote_address,
                        std::move(frame.bytes),
        [this, reply_basis, target_write_start](bool ok) mutable {
            target_write_timing_.End(target_write_start, now_ps_);
            if (!ok) {
                std::cerr << "udma-model: remote WRITE translation failed"
                          << " token=" << reply_basis.segment
                          << " address=0x" << std::hex
                          << reply_basis.remote_address << std::dec
                          << " bytes=" << reply_basis.transfer_length
                          << " request=" << reply_basis.request_id << '\n';
                ++ubase_errors_;
            }
            Frame ack{};
            ack.sequence = next_sequence_++;
            ack.source_eid = reply_basis.destination_eid;
            ack.destination_eid = reply_basis.source_eid;
            ack.source_port = reply_basis.destination_port;
            ack.destination_port = reply_basis.source_port;
            ack.operation = ok ? Frame::Operation::WriteAck : Frame::Operation::RmaError;
            ack.source_jetty = reply_basis.destination_jetty;
            ack.destination_jetty = reply_basis.source_jetty;
            ack.tpn = reply_basis.tpn;
            ack.request_id = reply_basis.request_id;
            ack.ta_ssn = reply_basis.ta_ssn;
            ack.transaction_offset = reply_basis.transaction_offset;
            ack.transfer_length = reply_basis.transfer_length;
            network_.Send(std::move(ack), [this](bool sent) {
                if (!sent) ++ubase_errors_;
                --remote_services_in_flight_;
            });
        }, config_.dma_max_outstanding);
}

void
UdmaModel::ReceiveReadRequest(Frame frame)
{
    if (!frame.remote_address || !frame.transfer_length ||
        frame.transfer_length - 1 > UINT64_MAX - frame.remote_address) {
        ++ubase_errors_;
        return;
    }
    const Frame reply_basis = frame;
    ++remote_services_in_flight_;
    ReadToken(frame.segment, frame.remote_address,
                       frame.transfer_length,
        [this, reply_basis](bool ok, std::vector<std::uint8_t> payload) mutable {
            const bool success = ok && payload.size() == reply_basis.transfer_length;
            if (!success) {
                std::cerr << "udma-model: remote READ translation failed"
                          << " token=" << reply_basis.segment
                          << " address=0x" << std::hex
                          << reply_basis.remote_address << std::dec
                          << " expected=" << reply_basis.transfer_length
                          << " received=" << payload.size()
                          << " request=" << reply_basis.request_id << '\n';
                ++ubase_errors_;
            }
            Frame response{};
            response.sequence = next_sequence_++;
            response.source_eid = reply_basis.destination_eid;
            response.destination_eid = reply_basis.source_eid;
            response.source_port = reply_basis.destination_port;
            response.destination_port = reply_basis.source_port;
            response.operation = success ? Frame::Operation::ReadResponse :
                                           Frame::Operation::RmaError;
            response.source_jetty = reply_basis.destination_jetty;
            response.destination_jetty = reply_basis.source_jetty;
            response.tpn = reply_basis.tpn;
            response.request_id = reply_basis.request_id;
            response.transfer_length = reply_basis.transfer_length;
            response.ta_ssn = reply_basis.ta_ssn;
            response.transaction_offset = reply_basis.transaction_offset;
            if (success) response.bytes = std::move(payload);
            network_.Send(std::move(response), [this](bool sent) {
                if (!sent) ++ubase_errors_;
                --remote_services_in_flight_;
            });
        }, config_.dma_max_outstanding);
}

void
UdmaModel::ReceiveWriteAck(Frame frame)
{
    auto found = pending_rma_.find(frame.request_id);
    if (found == pending_rma_.end() || found->second.opcode != 3) {
        ++ubase_errors_;
        return;
    }
    PendingRma& progress = found->second;
    if (frame.transfer_length == 0 ||
        progress.completed_tassns.count(frame.ta_ssn) != 0 ||
        frame.transaction_offset > progress.byte_count ||
        frame.transfer_length > progress.byte_count - frame.transaction_offset ||
        frame.transfer_length > progress.byte_count - progress.completed_bytes) {
        ++ubase_errors_;
        return;
    }
    auto& ranges = progress.response_ranges;
    const auto begin = frame.transaction_offset;
    const auto end = begin + frame.transfer_length;
    const auto next = std::lower_bound(ranges.begin(), ranges.end(), begin,
        [](const auto& range, std::uint32_t offset) { return range.first < offset; });
    if ((next != ranges.end() && next->first < end) ||
        (next != ranges.begin() && (next - 1)->second > begin)) {
        ++ubase_errors_;
        return;
    }
    ranges.insert(next, {begin, end});
    progress.completed_tassns.emplace(frame.ta_ssn, true);
    progress.completed_bytes += frame.transfer_length;
    if (progress.completed_bytes != progress.byte_count) return;
    const auto started = write_ack_starts_.find(frame.request_id);
    if (started != write_ack_starts_.end()) {
        write_ack_timing_.End(started->second, now_ps_);
        write_ack_starts_.erase(started);
    }
    const PendingRma pending = progress;
    pending_rma_.erase(found);
    CompleteSq(pending.jetty_id, pending.producer, pending.wqebbs,
               pending.completed_index, pending.opcode, pending.byte_count,
               pending.immediate, pending.completion);
}

void
UdmaModel::ReceiveReadResponse(Frame frame)
{
    auto found = pending_rma_.find(frame.request_id);
    if (found == pending_rma_.end() || found->second.opcode != 6 ||
        found->second.failed ||
        frame.bytes.size() != frame.transfer_length ||
        frame.transfer_length == 0 ||
        frame.transaction_offset > found->second.byte_count ||
        frame.transfer_length > found->second.byte_count - frame.transaction_offset ||
        frame.transfer_length > found->second.byte_count - found->second.accepted_bytes ||
        (found->second.byte_count && found->second.byte_count - 1ULL >
            std::numeric_limits<std::uint64_t>::max() - found->second.local_address) ||
        found->second.completed_tassns.count(frame.ta_ssn) != 0) {
        ++ubase_errors_;
        return;
    }
    const std::uint64_t request_id = frame.request_id;
    const std::uint32_t ta_ssn = frame.ta_ssn;
    const std::uint32_t transfer_length = frame.transfer_length;
    const std::uint32_t jetty_id = found->second.jetty_id;
    auto& ranges = found->second.response_ranges;
    const auto begin = frame.transaction_offset;
    const auto end = begin + transfer_length; // bounded by byte_count above
    const auto next = std::lower_bound(ranges.begin(), ranges.end(), begin,
        [](const auto& range, std::uint32_t offset) { return range.first < offset; });
    if ((next != ranges.end() && next->first < end) ||
        (next != ranges.begin() && (next - 1)->second > begin)) {
        ++ubase_errors_;
        return;
    }
    ranges.insert(next, {begin, end});
    // Reserve the TA before issuing asynchronous writes. Duplicate packets
    // must not submit a second DMA into the same destination while one is live.
    found->second.completed_tassns.emplace(ta_ssn, false);
    found->second.accepted_bytes += transfer_length;
    ++found->second.response_dma;
    ++read_response_dma_[jetty_id];
    WriteToken(found->second.local_token,
               found->second.local_address + frame.transaction_offset,
                        std::move(frame.bytes),
        [this, request_id, ta_ssn, transfer_length, jetty_id](bool ok) {
          // Keep both the request and queue resource owned until host DMA ends.
          auto finish_response = [this, request_id, ta_ssn, transfer_length, ok]() {
            auto current = pending_rma_.find(request_id);
            if (current == pending_rma_.end()) {
                ++ubase_errors_;
                return;
            }
            PendingRma& progress = current->second;
            --progress.response_dma;
            if (!ok) {
                progress.failed = true;
                ++ubase_errors_;
            } else {
                progress.completed_tassns[ta_ssn] = true;
                progress.completed_bytes += transfer_length;
            }
            if (progress.response_dma ||
                (!progress.failed && progress.completed_bytes != progress.byte_count)) return;
            const PendingRma pending = progress;
            pending_rma_.erase(current);
            CompleteSq(pending.jetty_id, pending.producer, pending.wqebbs,
                       pending.completed_index, pending.opcode,
                       pending.byte_count, pending.immediate,
                       pending.completion, pending.failed ? 5 : 0);
          };
          finish_response();
          const auto active = read_response_dma_.find(jetty_id);
          if (active != read_response_dma_.end() && --active->second == 0)
              read_response_dma_.erase(active);
          MaybeCompleteQueueDestroy();
        }, config_.dma_max_outstanding);
}

void
UdmaModel::ProcessReceiveQueue()
{
    if (receive_busy_ || receive_frames_.empty()) return;
    receive_busy_ = true;
    auto frame = std::make_shared<Frame>(std::move(receive_frames_.front()));
    receive_frames_.pop_front();
    ReceiveSend(std::move(frame));
}

void
UdmaModel::ReleaseReceive()
{
    active_receive_jfr_.reset();
    active_receive_jetty_.reset();
    receive_busy_ = false;
    MaybeCompleteQueueDestroy();
    ProcessReceiveQueue();
}

void
UdmaModel::ReceiveSend(std::shared_ptr<Frame> frame)
{
    auto fail = [this]() {
        ++ubase_errors_;
        ReleaseReceive();
    };
    auto jetty = jetty_contexts_.find(frame->destination_jetty);
    if (jetty == jetty_contexts_.end()) return fail();
    if (draining_queue_command_ == 0x07 &&
        draining_queue_id_ == frame->destination_jetty) return fail();
    auto jfr = jfr_contexts_.find(jetty->second.receive_queue);
    if (jfr == jfr_contexts_.end() || !jfr->second.producer_iova)
        return fail();
    const std::uint32_t jfr_id = jfr->first;
    if (draining_queue_command_ == 0x57 && draining_queue_id_ == jfr_id)
        return fail();
    active_receive_jfr_ = jfr_id;
    active_receive_jetty_ = frame->destination_jetty;
    const std::uint64_t pi = jfr->second.producer_iova;
    ReadToken(jfr->second.token, pi, 4,
        [this, frame = std::move(frame), jfr_id]
        (bool ok, std::vector<std::uint8_t> producer_bytes) mutable {
            auto fail = [this]() {
                ++ubase_errors_; ReleaseReceive();
            };
            auto jfr = jfr_contexts_.find(jfr_id);
            if (!ok || producer_bytes.size() != 4 || jfr == jfr_contexts_.end())
                return fail();
            const std::uint16_t producer =
                static_cast<std::uint16_t>(abi::Load32(producer_bytes.data()));
            if (static_cast<std::uint16_t>(jfr->second.consumer) == producer)
                return fail();
            const std::uint64_t address = jfr->second.index_iova +
                (jfr->second.consumer & (jfr->second.depth - 1U)) * 4;
            ReadToken(jfr->second.token, address, 4,
                [this, frame = std::move(frame), jfr_id]
                (bool index_ok, std::vector<std::uint8_t> index_bytes) mutable {
                    auto jfr = jfr_contexts_.find(jfr_id);
                    if (!index_ok || index_bytes.size() != 4 ||
                        jfr == jfr_contexts_.end()) {
                        ++ubase_errors_; ReleaseReceive(); return;
                    }
                    const std::uint32_t rqe = abi::Load32(index_bytes.data());
                    if (rqe >= jfr->second.depth) {
                        ++ubase_errors_; ReleaseReceive(); return;
                    }
                    ReceiveSge(std::move(frame), jfr_id, rqe, 0, 0);
                });
        });
}

void
UdmaModel::ReceiveSge(std::shared_ptr<Frame> frame, std::uint32_t jfr_id,
                      std::uint32_t rqe_index, std::uint32_t sge_index,
                      std::uint32_t copied)
{
    auto jfr = jfr_contexts_.find(jfr_id);
    if (jfr == jfr_contexts_.end()) {
        ++ubase_errors_;
        ReleaseReceive();
        return;
    }
    if (copied == frame->bytes.size())
        return FinishReceive(std::move(frame), jfr_id, rqe_index);
    if (sge_index >= jfr->second.max_sge) {
        ++ubase_errors_; ReleaseReceive(); return;
    }
    const std::uint64_t address = jfr->second.queue_iova +
        std::uint64_t(rqe_index) * jfr->second.entry_stride +
        std::uint64_t(sge_index) * abi::kSgeBytes;
    ReadToken(jfr->second.token, address, abi::kSgeBytes,
        [this, frame = std::move(frame), jfr_id, rqe_index, sge_index, copied]
        (bool ok, std::vector<std::uint8_t> bytes) mutable {
            if (!ok || bytes.size() != abi::kSgeBytes) {
                ++ubase_errors_; ReleaseReceive(); return;
            }
            const abi::Sge sge{abi::Load32(bytes.data()),
                               abi::Load32(bytes.data() + 4),
                               abi::Load64(bytes.data() + 8)};
            const std::uint32_t chunk = std::min<std::uint32_t>(
                sge.length, static_cast<std::uint32_t>(frame->bytes.size() - copied));
            if (!chunk || !sge.address) {
                ++ubase_errors_; ReleaseReceive(); return;
            }
            std::vector<std::uint8_t> payload(frame->bytes.begin() + copied,
                                               frame->bytes.begin() + copied + chunk);
            auto jfr = jfr_contexts_.find(jfr_id);
            if (jfr == jfr_contexts_.end()) {
                ++ubase_errors_; ReleaseReceive(); return;
            }
            WriteToken(jfr->second.payload_token, sge.address,
                       std::move(payload),
                [this, frame = std::move(frame), jfr_id, rqe_index,
                 sge_index, copied, chunk](bool write_ok) mutable {
                    if (!write_ok) {
                        ++ubase_errors_; ReleaseReceive(); return;
                    }
                    ReceiveSge(std::move(frame), jfr_id, rqe_index,
                               sge_index + 1, copied + chunk);
                }, config_.dma_max_outstanding);
        });
}

void
UdmaModel::FinishReceive(std::shared_ptr<Frame> frame, std::uint32_t jfr_id,
                         std::uint32_t rqe_index)
{
    auto jfr = jfr_contexts_.find(jfr_id);
    auto jetty = jetty_contexts_.find(frame->destination_jetty);
    if (jfr == jfr_contexts_.end() || jetty == jetty_contexts_.end()) {
        ++ubase_errors_;
        ReleaseReceive();
        return;
    }
    const std::uint32_t jfc_id = jetty->second.receive_completion_queue ?
        jetty->second.receive_completion_queue : jfr->second.completion_queue;
    WriteCqe(jfc_id, true, true, static_cast<std::uint8_t>(frame->operation),
             static_cast<std::uint16_t>(rqe_index), frame->destination_jetty,
             static_cast<std::uint32_t>(frame->bytes.size()), 0,
             frame->immediate,
        [this, jfr_id](bool ok) {
            auto jfr = jfr_contexts_.find(jfr_id);
            if (ok && jfr != jfr_contexts_.end()) ++jfr->second.consumer;
            if (!ok) ++ubase_errors_;
            ReleaseReceive();
        }, frame->source_jetty, frame->source_eid, frame->tpn);
}


} // namespace ubsim::device
