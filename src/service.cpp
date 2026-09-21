// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/service.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#include "fabric_partition_manager/encoding.hpp"
#include "fabric_partition_manager/version.hpp"
#include "socket.hpp"

namespace fabric_partition_manager {
namespace {

constexpr std::uint16_t kFlagResponse = 0x0001u;

void encode_attempt(CanonicalWriter& writer, const AttemptToken& token) {
  writer.text_id(token.id);
  writer.u64(token.sequence.value());
}

bool decode_attempt(CanonicalReader& reader, AttemptToken& token) noexcept {
  return reader.text_id(token.id) && reader.counter(token.sequence);
}

std::string protocol_failure(FrameDecodeStatus status) {
  std::string text("frame rejected: ");
  text.append(frame_decode_status_name(status));
  return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// Payload codecs
// ---------------------------------------------------------------------------

std::vector<std::byte> encode_merge_request(const MergeRequest& request, const Limits& limits) {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  encode_attempt(writer, request.attempt);
  writer.u64(request.expected_epoch.value());
  writer.text_id(request.subject);
  writer.text_id(request.left);
  writer.text_id(request.right);
  writer.text_id(request.requester);
  (void)limits;
  return bytes;
}

std::optional<MergeRequest> decode_merge_request(const std::vector<std::byte>& bytes,
                                                 const Limits& limits) {
  (void)limits;
  CanonicalReader reader(bytes.data(), bytes.size());
  MergeRequest request;
  if (!decode_attempt(reader, request.attempt) || !reader.counter(request.expected_epoch) ||
      !reader.text_id(request.subject) || !reader.text_id(request.left) ||
      !reader.text_id(request.right) || !reader.text_id(request.requester) ||
      !reader.exhausted()) {
    return std::nullopt;
  }
  return request;
}

std::vector<std::byte> encode_merge_ack(const MergeAckView& view, const Limits& limits) {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  const std::vector<std::byte> result = encode_operation_result(view.result, limits);
  writer.bytes(result.data(), result.size());
  writer.text_id(view.merged_lineage);
  writer.text_id(view.merged_partition);
  return bytes;
}

std::optional<MergeAckView> decode_merge_ack(const std::vector<std::byte>& bytes,
                                             const Limits& limits) {
  if (bytes.empty()) {
    return std::nullopt;
  }
  CanonicalReader head(bytes.data(), bytes.size());
  std::uint8_t verdict = 0;
  if (!head.u8(verdict) || verdict > decision_verdict_domain_max) {
    return std::nullopt;
  }
  // The operation result is a variable-length prefix; decode it, then read the
  // two identities that follow it.
  CanonicalReader reader(bytes.data(), bytes.size());
  OperationResult result;
  std::uint8_t raw_verdict = 0;
  if (!reader.u8(raw_verdict)) {
    return std::nullopt;
  }
  result.verdict = static_cast<DecisionVerdict>(raw_verdict);
  if (!reader.text_id(result.decision)) {
    return std::nullopt;
  }
  std::uint32_t reason_count = 0;
  if (!reader.u32(reason_count) || reason_count > limits.max_reasons_per_decision) {
    return std::nullopt;
  }
  result.reasons.reserve(reason_count);
  for (std::uint32_t index = 0; index < reason_count; ++index) {
    std::uint16_t code = 0;
    std::string_view subject;
    std::string_view detail;
    if (!reader.u16(code) || !reader.bounded_text(limits.max_text_bytes, subject) ||
        !reader.bounded_text(limits.max_text_bytes, detail)) {
      return std::nullopt;
    }
    Reason reason;
    reason.code = static_cast<ReasonCode>(code);
    reason.subject.assign(subject);
    reason.detail.assign(detail);
    result.reasons.push_back(std::move(reason));
  }
  MergeAckView view;
  view.result = std::move(result);
  if (!reader.text_id(view.merged_lineage) || !reader.text_id(view.merged_partition) ||
      !reader.exhausted()) {
    return std::nullopt;
  }
  return view;
}

std::vector<std::byte> encode_isolation_request(const IsolationRequest& request, bool clear,
                                                const Limits& limits) {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  encode_attempt(writer, request.attempt);
  writer.u64(request.expected_epoch.value());
  writer.text_id(request.lineage);
  writer.u16(static_cast<std::uint16_t>(request.cause));
  writer.u64(request.duration_ticks);
  writer.text_id(request.requester);
  writer.boolean(clear);
  (void)limits;
  return bytes;
}

std::optional<IsolationRequest> decode_isolation_request(const std::vector<std::byte>& bytes,
                                                         bool& clear, const Limits& limits) {
  (void)limits;
  CanonicalReader reader(bytes.data(), bytes.size());
  IsolationRequest request;
  std::uint16_t cause = 0;
  if (!decode_attempt(reader, request.attempt) || !reader.counter(request.expected_epoch) ||
      !reader.text_id(request.lineage) || !reader.u16(cause) ||
      !reader.u64(request.duration_ticks) || !reader.text_id(request.requester) ||
      !reader.boolean(clear) || !reader.exhausted()) {
    return std::nullopt;
  }
  request.cause = static_cast<ReasonCode>(cause);
  return request;
}

std::vector<std::byte> encode_revalidation_request(const RevalidationRequest& request) noexcept {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  encode_attempt(writer, request.attempt);
  writer.u64(request.expected_epoch.value());
  writer.text_id(request.requester);
  return bytes;
}

std::optional<RevalidationRequest> decode_revalidation_request(const std::vector<std::byte>& bytes,
                                                               const Limits& limits) {
  (void)limits;
  CanonicalReader reader(bytes.data(), bytes.size());
  RevalidationRequest request;
  if (!decode_attempt(reader, request.attempt) || !reader.counter(request.expected_epoch) ||
      !reader.text_id(request.requester) || !reader.exhausted()) {
    return std::nullopt;
  }
  return request;
}

std::vector<std::byte> encode_retire_request(const RetireRequest& request) noexcept {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  encode_attempt(writer, request.attempt);
  writer.u64(request.expected_epoch.value());
  writer.text_id(request.lineage);
  writer.text_id(request.requester);
  return bytes;
}

std::optional<RetireRequest> decode_retire_request(const std::vector<std::byte>& bytes,
                                                   const Limits& limits) {
  (void)limits;
  CanonicalReader reader(bytes.data(), bytes.size());
  RetireRequest request;
  if (!decode_attempt(reader, request.attempt) || !reader.counter(request.expected_epoch) ||
      !reader.text_id(request.lineage) || !reader.text_id(request.requester) ||
      !reader.exhausted()) {
    return std::nullopt;
  }
  return request;
}

std::vector<std::byte> encode_fence_list(const std::vector<FenceRecord>& fences,
                                         const Limits& limits) {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  const std::size_t count = std::min(fences.size(), limits.max_fence_records);
  writer.u32(static_cast<std::uint32_t>(count));
  for (std::size_t index = 0; index < count; ++index) {
    const std::vector<std::byte> entry = encode_fence(fences[index]);
    writer.u32(static_cast<std::uint32_t>(entry.size()));
    writer.bytes(entry.data(), entry.size());
  }
  return bytes;
}

std::optional<std::vector<FenceRecord>> decode_fence_list(const std::vector<std::byte>& bytes,
                                                          const Limits& limits) {
  CanonicalReader reader(bytes.data(), bytes.size());
  std::uint32_t count = 0;
  if (!reader.u32(count) || static_cast<std::size_t>(count) > limits.max_fence_records) {
    return std::nullopt;
  }
  std::vector<FenceRecord> fences;
  fences.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    std::uint32_t size = 0;
    if (!reader.u32(size) || size > limits.max_record_bytes) {
      return std::nullopt;
    }
    const std::byte* raw = nullptr;
    if (!reader.bytes(size, raw)) {
      return std::nullopt;
    }
    std::vector<std::byte> entry(raw, raw + size);
    const auto decoded = decode_fence(entry, limits);
    if (!decoded.has_value()) {
      return std::nullopt;
    }
    fences.push_back(*decoded);
  }
  if (!reader.exhausted()) {
    return std::nullopt;
  }
  return fences;
}

std::vector<std::byte> encode_lineage_list(const std::vector<LineageRecord>& records,
                                           const Limits& limits) {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  const std::size_t count = std::min(records.size(), limits.max_lineage_records);
  writer.u32(static_cast<std::uint32_t>(count));
  for (std::size_t index = 0; index < count; ++index) {
    const std::vector<std::byte> entry = encode_lineage(records[index]);
    writer.u32(static_cast<std::uint32_t>(entry.size()));
    writer.bytes(entry.data(), entry.size());
  }
  return bytes;
}

std::optional<std::vector<LineageRecord>> decode_lineage_list(const std::vector<std::byte>& bytes,
                                                              const Limits& limits) {
  CanonicalReader reader(bytes.data(), bytes.size());
  std::uint32_t count = 0;
  if (!reader.u32(count) || static_cast<std::size_t>(count) > limits.max_lineage_records) {
    return std::nullopt;
  }
  std::vector<LineageRecord> records;
  records.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    std::uint32_t size = 0;
    if (!reader.u32(size) || size > limits.max_record_bytes) {
      return std::nullopt;
    }
    const std::byte* raw = nullptr;
    if (!reader.bytes(size, raw)) {
      return std::nullopt;
    }
    std::vector<std::byte> entry(raw, raw + size);
    const auto decoded = decode_lineage(entry, limits);
    if (!decoded.has_value()) {
      return std::nullopt;
    }
    records.push_back(*decoded);
  }
  if (!reader.exhausted()) {
    return std::nullopt;
  }
  return records;
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

struct PartitionServer::Impl {
  struct Session {
    detail::Socket socket;
    std::thread thread;
    std::array<std::byte, session_handle_bytes> handle{};
    PublisherId publisher;
    PublisherBootId boot;
    std::uint64_t last_sequence = 0;
    std::uint64_t epoch = 0;
    bool established = false;
    bool finished = false;
  };

  PartitionRuntime* runtime = nullptr;
  ServerOptions options;
  detail::Socket listener;
  std::thread acceptor;
  std::atomic<bool> stopping{false};
  // The session table is the only state guarded by mutex. Every statistic is a
  // separate atomic, so no counter update can ever re-enter the session lock
  // from inside a helper that already holds it.
  mutable std::mutex mutex;
  std::vector<std::unique_ptr<Session>> sessions;
  std::atomic<std::uint64_t> accepted_sessions{0};
  std::atomic<std::uint64_t> rejected_sessions{0};
  std::atomic<std::uint64_t> completed_sessions{0};
  std::atomic<std::uint64_t> frames_received{0};
  std::atomic<std::uint64_t> frames_sent{0};
  std::atomic<std::uint64_t> protocol_errors{0};
  std::atomic<std::uint64_t> replay_rejections{0};
  std::atomic<std::uint64_t> binding_rejections{0};
  std::atomic<std::uint64_t> oversized_rejections{0};
  std::atomic<std::uint64_t> active_sessions{0};
  std::atomic<std::uint64_t> peak_sessions{0};
  std::uint16_t bound_port = 0;
  bool started = false;

  [[nodiscard]] bool send_frame(Session& session, MessageType type, std::uint64_t sequence,
                                std::uint64_t epoch, const std::vector<std::byte>& payload) {
    FrameHeader header;
    header.version = wire_protocol_version;
    header.type = static_cast<std::uint16_t>(type);
    header.flags = kFlagResponse;
    header.payload_length = static_cast<std::uint32_t>(payload.size());
    header.sequence = sequence;
    header.epoch = epoch;
    header.session = session.handle;
    const std::vector<std::byte> frame = encode_frame(header, payload, options.limits);
    if (frame.empty()) {
      return false;
    }
    std::string error;
    if (!session.socket.send_all(frame.data(), frame.size(), error)) {
      return false;
    }
    frames_sent.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void fail_session(Session& session, std::uint64_t sequence, std::string_view detail) {
    OperationResult result;
    result.verdict = DecisionVerdict::Invalid;
    Reason reason;
    reason.code = ReasonCode::InvalidRequest;
    reason.subject.assign(detail.substr(0, std::min<std::size_t>(detail.size(), 64)));
    result.reasons.push_back(std::move(reason));
    const std::vector<std::byte> payload = encode_operation_result(result, options.limits);
    (void)send_frame(session, MessageType::ProtocolError, sequence, session.epoch, payload);
    protocol_errors.fetch_add(1, std::memory_order_relaxed);
    session.socket.shutdown_both();
  }

  [[nodiscard]] std::vector<std::byte> handle_request(MessageType type,
                                                      const std::vector<std::byte>& payload,
                                                      bool& supported) {
    supported = true;
    OperationResult result;
    switch (type) {
      case MessageType::PublishEvidence: {
        const auto evidence = decode_evidence(payload, options.limits);
        if (!evidence.has_value()) {
          supported = false;
          return {};
        }
        const Decision decision = runtime->ingest_evidence(*evidence);
        result.verdict = decision.verdict;
        result.decision = decision.id;
        result.reasons = decision.reasons;
        return encode_operation_result(result, options.limits);
      }
      case MessageType::AdoptTopology: {
        const auto definition = decode_topology(payload, options.limits);
        if (!definition.has_value()) {
          supported = false;
          return {};
        }
        const Decision decision = runtime->adopt_topology(*definition);
        result.verdict = decision.verdict;
        result.decision = decision.id;
        result.reasons = decision.reasons;
        return encode_operation_result(result, options.limits);
      }
      case MessageType::SetPolicy: {
        const auto policy = decode_policy(payload, options.limits);
        if (!policy.has_value()) {
          supported = false;
          return {};
        }
        const Decision decision = runtime->set_policy(*policy);
        result.verdict = decision.verdict;
        result.decision = decision.id;
        result.reasons = decision.reasons;
        return encode_operation_result(result, options.limits);
      }
      case MessageType::Assess: {
        (void)runtime->assess();
        const AssessmentView view = make_assessment_view(runtime->last_assessment(),
                                                         runtime->status());
        return encode_assessment_view(view, options.limits);
      }
      case MessageType::QueryStatus:
        return encode_status_view(make_status_view(runtime->status()));
      case MessageType::QueryPartitions: {
        const PartitionAssessment& assessment = runtime->last_assessment();
        AssessmentView view;
        view.generation = assessment.generation;
        view.confirmed = assessment.confirmed;
        view.decision = assessment.decision.id;
        view.verdict = assessment.decision.verdict;
        for (const Partition& partition : assessment.partitions) {
          PartitionView entry;
          entry.id = partition.id;
          entry.lineage = partition.lineage;
          entry.generation = partition.generation;
          entry.member_count = partition.member_count();
          entry.certainty = partition.certainty;
          entry.lifecycle = partition.lifecycle;
          entry.authority_class = partition.authority.authority_class;
          entry.basis = partition.authority.basis;
          entry.application_state = partition.authority.application_state;
          entry.authorized_bits = partition.authority.authorized.bits();
          entry.eligible_bits = partition.authority.eligible.bits();
          entry.observed_bits = partition.authority.observed.bits();
          entry.authority_sequence = partition.authority_sequence.value();
          entry.authoritative = partition.authoritative;
          entry.pending_merge = !partition.pending_merge_parents.empty();
          view.partitions.push_back(entry);
        }
        return encode_assessment_view(view, options.limits);
      }
      case MessageType::QueryFences:
        return encode_fence_list(runtime->fences(), options.limits);
      case MessageType::QueryLineage:
        return encode_lineage_list(runtime->lineage_records(), options.limits);
      case MessageType::Merge: {
        const auto request = decode_merge_request(payload, options.limits);
        if (!request.has_value()) {
          supported = false;
          return {};
        }
        const MergeOutcome outcome = runtime->request_merge(*request);
        MergeAckView view;
        view.result.verdict = outcome.verdict;
        view.result.decision = outcome.decision.id;
        view.result.reasons = outcome.decision.reasons;
        view.merged_lineage = outcome.merged_lineage;
        view.merged_partition = outcome.merged_partition;
        return encode_merge_ack(view, options.limits);
      }
      case MessageType::Isolate: {
        bool clear = false;
        const auto request = decode_isolation_request(payload, clear, options.limits);
        if (!request.has_value()) {
          supported = false;
          return {};
        }
        const IsolationOutcome outcome =
            clear ? runtime->clear_isolation(*request) : runtime->isolate(*request);
        result.verdict = outcome.verdict;
        result.decision = outcome.decision.id;
        result.reasons = outcome.decision.reasons;
        return encode_operation_result(result, options.limits);
      }
      case MessageType::Revalidate: {
        const auto request = decode_revalidation_request(payload, options.limits);
        if (!request.has_value()) {
          supported = false;
          return {};
        }
        const RevalidationOutcome outcome = runtime->revalidate(*request);
        result.verdict = outcome.verdict;
        result.decision = outcome.decision.id;
        result.reasons = outcome.decision.reasons;
        return encode_operation_result(result, options.limits);
      }
      case MessageType::Retire: {
        const auto request = decode_retire_request(payload, options.limits);
        if (!request.has_value()) {
          supported = false;
          return {};
        }
        const Decision decision = runtime->retire(*request);
        result.verdict = decision.verdict;
        result.decision = decision.id;
        result.reasons = decision.reasons;
        return encode_operation_result(result, options.limits);
      }
      default:
        supported = false;
        return {};
    }
  }

  void serve(Session& session) {
    std::vector<std::byte> buffer;
    buffer.reserve(4096);
    // Heap allocated so the session stack frame stays small.
    std::vector<std::byte> chunk(16 * 1024, std::byte{0});
    std::uint64_t response_sequence = 0;
    for (;;) {
      if (stopping.load(std::memory_order_acquire)) {
        return;
      }
      FrameDecodeResult decoded = decode_frame(buffer.data(), buffer.size(), options.limits);
      if (decoded.status == FrameDecodeStatus::NeedMore) {
        if (buffer.size() > options.limits.max_frame_bytes) {
          oversized_rejections.fetch_add(1, std::memory_order_relaxed);
          fail_session(session, response_sequence, "the receive buffer exceeded the frame bound");
          return;
        }
        std::string error;
        const int read = session.socket.recv_some(chunk.data(), chunk.size(), error);
        if (read == 0) {
          return;
        }
        if (read < 0) {
          return;
        }
        buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + read);
        continue;
      }
      if (decoded.status != FrameDecodeStatus::Ok) {
        fail_session(session, response_sequence, protocol_failure(decoded.status));
        return;
      }
      std::vector<std::byte> remaining(buffer.begin() + static_cast<std::ptrdiff_t>(decoded.consumed),
                                       buffer.end());
      buffer.swap(remaining);
      frames_received.fetch_add(1, std::memory_order_relaxed);

      const MessageType type = static_cast<MessageType>(decoded.header.type);
      if (!session.established) {
        if (type != MessageType::Hello) {
          binding_rejections.fetch_add(1, std::memory_order_relaxed);
          fail_session(session, response_sequence, "the first frame must be a hello");
          return;
        }
        const auto hello = decode_hello_request(decoded.payload, options.limits);
        if (!hello.has_value()) {
          fail_session(session, response_sequence, "the hello payload could not be decoded");
          return;
        }
        if (!hello->expected_epoch.is_zero() &&
            !(hello->expected_epoch == runtime->epoch())) {
          HelloResponse response;
          response.accepted = false;
          response.epoch = runtime->epoch();
          response.boot = runtime->boot();
          response.protocol_version = wire_protocol_version;
          Reason reason;
          reason.code = ReasonCode::EpochMismatchReason;
          reason.detail = "the requested epoch is not current";
          response.reasons.push_back(std::move(reason));
          (void)send_frame(session, MessageType::HelloAck, response_sequence, response.epoch.value(),
                           encode_hello_response(response, options.limits));
          rejected_sessions.fetch_add(1, std::memory_order_relaxed);
          session.socket.shutdown_both();
          return;
        }
        session.handle = {};
        detail::fill_session_handle(session.handle);
        session.publisher = hello->publisher;
        session.boot = hello->boot;
        session.epoch = runtime->epoch().value();
        session.established = true;
        HelloResponse response;
        response.session = session.handle;
        response.epoch = runtime->epoch();
        response.boot = runtime->boot();
        response.protocol_version = wire_protocol_version;
        response.accepted = true;
        Reason reason;
        reason.code = ReasonCode::EvidenceAccepted;
        reason.detail = "session established";
        response.reasons.push_back(std::move(reason));
        if (!send_frame(session, MessageType::HelloAck, response_sequence, session.epoch,
                        encode_hello_response(response, options.limits))) {
          return;
        }
        accepted_sessions.fetch_add(1, std::memory_order_relaxed);
        continue;
      }

      if (decoded.header.session != session.handle) {
        binding_rejections.fetch_add(1, std::memory_order_relaxed);
        fail_session(session, response_sequence,
                     "the frame names a session handle that this connection does not own");
        return;
      }
      if (decoded.header.epoch != session.epoch ||
          decoded.header.epoch != runtime->epoch().value()) {
        binding_rejections.fetch_add(1, std::memory_order_relaxed);
        fail_session(session, response_sequence,
                     "the frame names an epoch that is not the established epoch");
        return;
      }
      if (decoded.header.sequence <= session.last_sequence) {
        replay_rejections.fetch_add(1, std::memory_order_relaxed);
        fail_session(session, response_sequence,
                     "the frame sequence did not advance past the last accepted sequence");
        return;
      }
      session.last_sequence = decoded.header.sequence;

      if (type == MessageType::Goodbye) {
        return;
      }
      bool supported = true;
      const std::vector<std::byte> payload = handle_request(type, decoded.payload, supported);
      if (!supported) {
        fail_session(session, response_sequence, "the request type or payload is not supported");
        return;
      }
      MessageType response_type = MessageType::ProtocolError;
      for (std::uint16_t candidate = 1; candidate <= message_type_domain_max; ++candidate) {
        if (message_is_response_to(static_cast<MessageType>(candidate), type)) {
          response_type = static_cast<MessageType>(candidate);
          break;
        }
      }
      if (response_type == MessageType::ProtocolError) {
        fail_session(session, response_sequence, "the request has no response type");
        return;
      }
      if (!send_frame(session, response_type, response_sequence, session.epoch, payload)) {
        return;
      }
    }
  }

  void accept_loop() {
    while (!stopping.load(std::memory_order_acquire)) {
      std::string error;
      if (!listener.wait_readable(options.accept_poll_millis, error)) {
        continue;
      }
      detail::Socket accepted;
      if (!listener.accept_from(accepted, error)) {
        continue;
      }
      auto session = std::make_unique<Session>();
      session->socket = std::move(accepted);
      const std::uint64_t active = active_sessions.load(std::memory_order_relaxed);
      if (active >= options.limits.max_sessions) {
        rejected_sessions.fetch_add(1, std::memory_order_relaxed);
        session->socket.shutdown_both();
        session->socket.close();
        continue;
      }
      const std::uint64_t now_active = active + 1;
      active_sessions.store(now_active, std::memory_order_relaxed);
      std::uint64_t peak = peak_sessions.load(std::memory_order_relaxed);
      while (now_active > peak &&
             !peak_sessions.compare_exchange_weak(peak, now_active, std::memory_order_relaxed)) {
      }
      Session* raw = session.get();
      session->thread = std::thread([this, raw]() {
        serve(*raw);
        raw->finished = true;
        completed_sessions.fetch_add(1, std::memory_order_relaxed);
        active_sessions.fetch_sub(1, std::memory_order_relaxed);
      });
      {
        const std::lock_guard<std::mutex> guard(mutex);
        sessions.push_back(std::move(session));
      }
      if (sessions.size() > options.limits.max_sessions) {
        reap(false);
      }
    }
    reap(true);
  }

  void reap(bool join_all) {
    std::vector<std::unique_ptr<Session>> reclaimed;
    {
      const std::lock_guard<std::mutex> guard(mutex);
      for (std::size_t index = 0; index < sessions.size();) {
        const bool done = join_all || sessions[index]->finished;
        if (!done) {
          ++index;
          continue;
        }
        sessions[index]->socket.shutdown_both();
        reclaimed.push_back(std::move(sessions[index]));
        sessions.erase(sessions.begin() + static_cast<std::ptrdiff_t>(index));
      }
    }
    for (std::unique_ptr<Session>& session : reclaimed) {
      if (session->thread.joinable()) {
        session->thread.join();
      }
      session->socket.close();
    }
  }
};

PartitionServer::PartitionServer(PartitionRuntime& runtime, const ServerOptions& options)
    : impl_(std::make_unique<Impl>()) {
  impl_->runtime = &runtime;
  impl_->options = options;
}

PartitionServer::~PartitionServer() { stop(); }

bool PartitionServer::start(std::string& error) {
  if (impl_->started) {
    error = "the server is already started";
    return false;
  }
  std::string listen_error;
  if (!impl_->listener.listen_on(impl_->options.bind_address, impl_->options.port,
                                 impl_->bound_port, listen_error)) {
    error = listen_error;
    return false;
  }
  impl_->started = true;
  impl_->acceptor = std::thread([this]() { impl_->accept_loop(); });
  return true;
}

void PartitionServer::stop() {
  if (!impl_ || !impl_->started) {
    return;
  }
  impl_->stopping.store(true, std::memory_order_release);
  // Releasing blocked reads before joining is what makes shutdown prompt.
  {
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    for (std::unique_ptr<Impl::Session>& session : impl_->sessions) {
      session->socket.shutdown_both();
    }
  }
  if (impl_->acceptor.joinable()) {
    impl_->acceptor.join();
  }
  impl_->reap(true);
  impl_->listener.close();
  impl_->started = false;
}

std::uint16_t PartitionServer::port() const { return impl_->bound_port; }

std::string PartitionServer::endpoint() const {
  return impl_->options.bind_address + ":" + std::to_string(impl_->bound_port);
}

ServerStats PartitionServer::stats() const {
  ServerStats stats;
  stats.accepted_sessions = impl_->accepted_sessions.load(std::memory_order_relaxed);
  stats.rejected_sessions = impl_->rejected_sessions.load(std::memory_order_relaxed);
  stats.completed_sessions = impl_->completed_sessions.load(std::memory_order_relaxed);
  stats.frames_received = impl_->frames_received.load(std::memory_order_relaxed);
  stats.frames_sent = impl_->frames_sent.load(std::memory_order_relaxed);
  stats.protocol_errors = impl_->protocol_errors.load(std::memory_order_relaxed);
  stats.replay_rejections = impl_->replay_rejections.load(std::memory_order_relaxed);
  stats.binding_rejections = impl_->binding_rejections.load(std::memory_order_relaxed);
  stats.oversized_rejections = impl_->oversized_rejections.load(std::memory_order_relaxed);
  stats.active_sessions = impl_->active_sessions.load(std::memory_order_relaxed);
  stats.peak_sessions = impl_->peak_sessions.load(std::memory_order_relaxed);
  return stats;
}

bool PartitionServer::running() const { return impl_->started; }

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

struct PartitionClient::Impl {
  ClientOptions options;
  detail::Socket socket;
  std::array<std::byte, session_handle_bytes> handle{};
  CoordinatorEpoch epoch;
  CoordinatorBootId boot;
  SessionId session;
  std::uint64_t sequence = 0;
  bool connected = false;

  // Performs one request/response exchange. The handshake is itself an exchange,
  // so this must not require an established session; the public wrapper is what
  // refuses to act before the session exists.
  [[nodiscard]] ClientResponse exchange(MessageType request_type,
                                        const std::vector<std::byte>& payload) {
    ClientResponse response;
    response.type = request_type;
    FrameHeader header;
    header.version = wire_protocol_version;
    header.type = static_cast<std::uint16_t>(request_type);
    header.flags = 0;
    header.payload_length = static_cast<std::uint32_t>(payload.size());
    header.sequence = ++sequence;
    header.epoch = epoch.value();
    header.session = handle;
    const std::vector<std::byte> frame = encode_frame(header, payload, options.limits);
    if (frame.empty()) {
      response.error = "the request frame exceeds the configured bound";
      return response;
    }
    std::string error;
    if (!socket.send_all(frame.data(), frame.size(), error)) {
      response.error = "send failed: " + error;
      return response;
    }
    std::vector<std::byte> buffer;
    std::vector<std::byte> chunk(16 * 1024, std::byte{0});
    for (;;) {
      FrameDecodeResult decoded = decode_frame(buffer.data(), buffer.size(), options.limits);
      if (decoded.status == FrameDecodeStatus::Ok) {
        // The handshake response carries the handle the server just issued, so
        // it is exempt from the binding check. Every later response must echo
        // exactly the handle this session owns.
        if (request_type != MessageType::Hello && decoded.header.session != handle) {
          response.error = "the response names a different session handle";
          return response;
        }
        response.ok = true;
        response.type = static_cast<MessageType>(decoded.header.type);
        response.payload = std::move(decoded.payload);
        return response;
      }
      if (decoded.status != FrameDecodeStatus::NeedMore) {
        response.error = protocol_failure(decoded.status);
        return response;
      }
      if (buffer.size() > options.limits.max_frame_bytes) {
        response.error = "the receive buffer exceeded the frame bound";
        return response;
      }
      const int read = socket.recv_some(chunk.data(), chunk.size(), error);
      if (read == 0) {
        response.error = "the peer closed the connection";
        return response;
      }
      if (read < 0) {
        response.error = "receive failed: " + error;
        return response;
      }
      buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + read);
    }
  }
};

PartitionClient::PartitionClient(const ClientOptions& options) : impl_(std::make_unique<Impl>()) {
  impl_->options = options;
}

PartitionClient::~PartitionClient() { close(); }

bool PartitionClient::connect(const PublisherId& publisher, const PublisherBootId& boot,
                              const CoordinatorEpoch& expected_epoch, std::string& error) {
  if (impl_->connected) {
    error = "the client is already connected";
    return false;
  }
  if (!impl_->socket.connect_to(impl_->options.address, impl_->options.port, error)) {
    return false;
  }
  HelloRequest request;
  request.publisher = publisher;
  request.boot = boot;
  request.expected_epoch = expected_epoch;
  request.provenance = Provenance::from_validated("client");
  const std::vector<std::byte> payload = encode_hello_request(request, impl_->options.limits);
  const ClientResponse response = impl_->exchange(MessageType::Hello, payload);
  if (!response.ok) {
    error = response.error;
    impl_->socket.close();
    return false;
  }
  if (response.type != MessageType::HelloAck) {
    error = "the server did not answer the handshake";
    impl_->socket.close();
    return false;
  }
  const auto decoded = decode_hello_response(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the handshake response could not be decoded";
    impl_->socket.close();
    return false;
  }
  if (!decoded->accepted) {
    error = "the server refused the session";
    impl_->socket.close();
    return false;
  }
  impl_->handle = decoded->session;
  impl_->epoch = decoded->epoch;
  impl_->boot = decoded->boot;
  std::string handle_text("s-");
  {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (const std::byte value : impl_->handle) {
      const auto raw = static_cast<unsigned>(value);
      handle_text.push_back(kDigits[(raw >> 4) & 0xFu]);
      handle_text.push_back(kDigits[raw & 0xFu]);
    }
  }
  impl_->session = SessionId::from_validated(handle_text);
  impl_->connected = true;
  return true;
}

void PartitionClient::close() {
  if (!impl_ || !impl_->connected) {
    return;
  }
  FrameHeader header;
  header.version = wire_protocol_version;
  header.type = static_cast<std::uint16_t>(MessageType::Goodbye);
  header.sequence = ++impl_->sequence;
  header.epoch = impl_->epoch.value();
  header.session = impl_->handle;
  const std::vector<std::byte> frame = encode_frame(header, {}, impl_->options.limits);
  std::string error;
  if (!frame.empty()) {
    (void)impl_->socket.send_all(frame.data(), frame.size(), error);
  }
  impl_->socket.shutdown_both();
  impl_->socket.close();
  impl_->connected = false;
}

bool PartitionClient::connected() const { return impl_->connected; }
CoordinatorEpoch PartitionClient::epoch() const { return impl_->epoch; }
CoordinatorBootId PartitionClient::boot() const { return impl_->boot; }
SessionId PartitionClient::session() const { return impl_->session; }

ClientResponse PartitionClient::exchange(MessageType request_type,
                                         const std::vector<std::byte>& payload) {
  ClientResponse response;
  response.type = request_type;
  if (!impl_->connected) {
    response.error = "the client is not connected";
    return response;
  }
  return impl_->exchange(request_type, payload);
}

std::optional<OperationResult> PartitionClient::publish_evidence(
    const ReachabilityEvidence& evidence, std::string& error) {
  const ClientResponse response =
      exchange(MessageType::PublishEvidence, encode_evidence(evidence, impl_->options.limits));
  if (!response.ok) {
    error = response.error;
    return std::nullopt;
  }
  if (response.type != MessageType::PublishEvidenceAck) {
    error = "unexpected response type";
    return std::nullopt;
  }
  const auto decoded = decode_operation_result(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the response payload could not be decoded";
    return std::nullopt;
  }
  return decoded;
}

std::optional<OperationResult> PartitionClient::adopt_topology(const TopologyDefinition& definition,
                                                               std::string& error) {
  const ClientResponse response =
      exchange(MessageType::AdoptTopology, encode_topology(definition));
  if (!response.ok) {
    error = response.error;
    return std::nullopt;
  }
  const auto decoded = decode_operation_result(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the response payload could not be decoded";
    return std::nullopt;
  }
  return decoded;
}

std::optional<OperationResult> PartitionClient::set_policy(const PartitionPolicy& policy,
                                                           std::string& error) {
  const ClientResponse response = exchange(MessageType::SetPolicy, encode_policy(policy));
  if (!response.ok) {
    error = response.error;
    return std::nullopt;
  }
  const auto decoded = decode_operation_result(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the response payload could not be decoded";
    return std::nullopt;
  }
  return decoded;
}

std::optional<AssessmentView> PartitionClient::assess(std::string& error) {
  const ClientResponse response = exchange(MessageType::Assess, {});
  if (!response.ok) {
    error = response.error;
    return std::nullopt;
  }
  const auto decoded = decode_assessment_view(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the assessment payload could not be decoded";
    return std::nullopt;
  }
  return decoded;
}

std::optional<StatusView> PartitionClient::status(std::string& error) {
  const ClientResponse response = exchange(MessageType::QueryStatus, {});
  if (!response.ok) {
    error = response.error;
    return std::nullopt;
  }
  const auto decoded = decode_status_view(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the status payload could not be decoded";
    return std::nullopt;
  }
  return decoded;
}

std::optional<std::vector<PartitionView>> PartitionClient::partitions(std::string& error) {
  const ClientResponse response = exchange(MessageType::QueryPartitions, {});
  if (!response.ok) {
    error = response.error;
    return std::nullopt;
  }
  const auto decoded = decode_assessment_view(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the partition payload could not be decoded";
    return std::nullopt;
  }
  return decoded->partitions;
}

std::optional<std::vector<FenceRecord>> PartitionClient::fences(std::string& error) {
  const ClientResponse response = exchange(MessageType::QueryFences, {});
  if (!response.ok) {
    error = response.error;
    return std::nullopt;
  }
  const auto decoded = decode_fence_list(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the fence payload could not be decoded";
    return std::nullopt;
  }
  return decoded;
}

std::optional<std::vector<LineageRecord>> PartitionClient::lineage(std::string& error) {
  const ClientResponse response = exchange(MessageType::QueryLineage, {});
  if (!response.ok) {
    error = response.error;
    return std::nullopt;
  }
  const auto decoded = decode_lineage_list(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the lineage payload could not be decoded";
    return std::nullopt;
  }
  return decoded;
}

std::optional<MergeAckView> PartitionClient::request_merge(const MergeRequest& request,
                                                           std::string& error) {
  const ClientResponse response =
      exchange(MessageType::Merge, encode_merge_request(request, impl_->options.limits));
  if (!response.ok) {
    error = response.error;
    return std::nullopt;
  }
  const auto decoded = decode_merge_ack(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the merge payload could not be decoded";
    return std::nullopt;
  }
  return decoded;
}

std::optional<OperationResult> PartitionClient::isolate(const IsolationRequest& request,
                                                        std::string& error) {
  const ClientResponse response =
      exchange(MessageType::Isolate,
               encode_isolation_request(request, false, impl_->options.limits));
  if (!response.ok) {
    error = response.error;
    return std::nullopt;
  }
  const auto decoded = decode_operation_result(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the response payload could not be decoded";
    return std::nullopt;
  }
  return decoded;
}

std::optional<OperationResult> PartitionClient::clear_isolation(const IsolationRequest& request,
                                                                std::string& error) {
  const ClientResponse response =
      exchange(MessageType::Isolate,
               encode_isolation_request(request, true, impl_->options.limits));
  if (!response.ok) {
    error = response.error;
    return std::nullopt;
  }
  const auto decoded = decode_operation_result(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the response payload could not be decoded";
    return std::nullopt;
  }
  return decoded;
}

std::optional<OperationResult> PartitionClient::revalidate(const RevalidationRequest& request,
                                                           std::string& error) {
  const ClientResponse response =
      exchange(MessageType::Revalidate, encode_revalidation_request(request));
  if (!response.ok) {
    error = response.error;
    return std::nullopt;
  }
  const auto decoded = decode_operation_result(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the response payload could not be decoded";
    return std::nullopt;
  }
  return decoded;
}

std::optional<OperationResult> PartitionClient::retire(const RetireRequest& request,
                                                       std::string& error) {
  const ClientResponse response = exchange(MessageType::Retire, encode_retire_request(request));
  if (!response.ok) {
    error = response.error;
    return std::nullopt;
  }
  const auto decoded = decode_operation_result(response.payload, impl_->options.limits);
  if (!decoded.has_value()) {
    error = "the response payload could not be decoded";
    return std::nullopt;
  }
  return decoded;
}

}  // namespace fabric_partition_manager
