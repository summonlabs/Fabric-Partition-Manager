// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/protocol.hpp"

#include <algorithm>
#include <cstring>

#include "fabric_partition_manager/encoding.hpp"
#include "fabric_partition_manager/version.hpp"

namespace fabric_partition_manager {
namespace {

constexpr std::uint16_t kFlagResponse = 0x0001u;
constexpr std::uint16_t kFlagMask = kFlagResponse;

[[nodiscard]] bool valid_capability_bits(std::uint32_t bits) noexcept {
  return (bits & ~((1u << authority_capability_count) - 1u)) == 0u;
}

void encode_reason(CanonicalWriter& writer, const Reason& reason) {
  writer.u16(static_cast<std::uint16_t>(reason.code));
  writer.text(reason.subject);
  writer.text(reason.detail);
}

bool decode_reason(CanonicalReader& reader, const Limits& limits, Reason& out) noexcept {
  std::uint16_t code = 0;
  std::string_view subject;
  std::string_view detail;
  if (!reader.u16(code) || !reader.bounded_text(limits.max_text_bytes, subject) ||
      !reader.bounded_text(limits.max_text_bytes, detail)) {
    return false;
  }
  out.code = static_cast<ReasonCode>(code);
  out.subject.assign(subject);
  out.detail.assign(detail);
  return true;
}

void encode_reasons(CanonicalWriter& writer, const std::vector<Reason>& reasons,
                    const Limits& limits) {
  const std::size_t count = std::min(reasons.size(), limits.max_reasons_per_decision);
  writer.u32(static_cast<std::uint32_t>(count));
  for (std::size_t index = 0; index < count; ++index) {
    encode_reason(writer, reasons[index]);
  }
}

bool decode_reasons(CanonicalReader& reader, const Limits& limits,
                    std::vector<Reason>& out) noexcept {
  std::uint32_t count = 0;
  if (!reader.u32(count) || static_cast<std::size_t>(count) > limits.max_reasons_per_decision) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    Reason reason;
    if (!decode_reason(reader, limits, reason)) {
      return false;
    }
    out.push_back(std::move(reason));
  }
  return true;
}

bool decode_enum8(CanonicalReader& reader, std::uint8_t domain_max, std::uint8_t& out) noexcept {
  std::uint8_t value = 0;
  if (!reader.u8(value) || value > domain_max) {
    return false;
  }
  out = value;
  return true;
}

}  // namespace

std::string_view message_type_name(MessageType value) noexcept {
  switch (value) {
    case MessageType::Hello: return "HELLO";
    case MessageType::HelloAck: return "HELLO_ACK";
    case MessageType::Goodbye: return "GOODBYE";
    case MessageType::ProtocolError: return "PROTOCOL_ERROR";
    case MessageType::PublishEvidence: return "PUBLISH_EVIDENCE";
    case MessageType::PublishEvidenceAck: return "PUBLISH_EVIDENCE_ACK";
    case MessageType::AdoptTopology: return "ADOPT_TOPOLOGY";
    case MessageType::AdoptTopologyAck: return "ADOPT_TOPOLOGY_ACK";
    case MessageType::SetPolicy: return "SET_POLICY";
    case MessageType::SetPolicyAck: return "SET_POLICY_ACK";
    case MessageType::Assess: return "ASSESS";
    case MessageType::AssessAck: return "ASSESS_ACK";
    case MessageType::QueryStatus: return "QUERY_STATUS";
    case MessageType::QueryStatusAck: return "QUERY_STATUS_ACK";
    case MessageType::QueryPartitions: return "QUERY_PARTITIONS";
    case MessageType::QueryPartitionsAck: return "QUERY_PARTITIONS_ACK";
    case MessageType::QueryFences: return "QUERY_FENCES";
    case MessageType::QueryFencesAck: return "QUERY_FENCES_ACK";
    case MessageType::QueryLineage: return "QUERY_LINEAGE";
    case MessageType::QueryLineageAck: return "QUERY_LINEAGE_ACK";
    case MessageType::Merge: return "MERGE";
    case MessageType::MergeAck: return "MERGE_ACK";
    case MessageType::Isolate: return "ISOLATE";
    case MessageType::IsolateAck: return "ISOLATE_ACK";
    case MessageType::Revalidate: return "REVALIDATE";
    case MessageType::RevalidateAck: return "REVALIDATE_ACK";
    case MessageType::Retire: return "RETIRE";
    case MessageType::RetireAck: return "RETIRE_ACK";
  }
  return "UNRECOGNIZED";
}

bool message_type_in_domain(std::uint16_t value) noexcept {
  switch (static_cast<MessageType>(value)) {
    case MessageType::Hello:
    case MessageType::HelloAck:
    case MessageType::Goodbye:
    case MessageType::ProtocolError:
    case MessageType::PublishEvidence:
    case MessageType::PublishEvidenceAck:
    case MessageType::AdoptTopology:
    case MessageType::AdoptTopologyAck:
    case MessageType::SetPolicy:
    case MessageType::SetPolicyAck:
    case MessageType::Assess:
    case MessageType::AssessAck:
    case MessageType::QueryStatus:
    case MessageType::QueryStatusAck:
    case MessageType::QueryPartitions:
    case MessageType::QueryPartitionsAck:
    case MessageType::QueryFences:
    case MessageType::QueryFencesAck:
    case MessageType::QueryLineage:
    case MessageType::QueryLineageAck:
    case MessageType::Merge:
    case MessageType::MergeAck:
    case MessageType::Isolate:
    case MessageType::IsolateAck:
    case MessageType::Revalidate:
    case MessageType::RevalidateAck:
    case MessageType::Retire:
    case MessageType::RetireAck:
      return true;
  }
  return false;
}

bool message_is_response_to(MessageType response, MessageType request) noexcept {
  switch (request) {
    case MessageType::Hello: return response == MessageType::HelloAck;
    case MessageType::PublishEvidence: return response == MessageType::PublishEvidenceAck;
    case MessageType::AdoptTopology: return response == MessageType::AdoptTopologyAck;
    case MessageType::SetPolicy: return response == MessageType::SetPolicyAck;
    case MessageType::Assess: return response == MessageType::AssessAck;
    case MessageType::QueryStatus: return response == MessageType::QueryStatusAck;
    case MessageType::QueryPartitions: return response == MessageType::QueryPartitionsAck;
    case MessageType::QueryFences: return response == MessageType::QueryFencesAck;
    case MessageType::QueryLineage: return response == MessageType::QueryLineageAck;
    case MessageType::Merge: return response == MessageType::MergeAck;
    case MessageType::Isolate: return response == MessageType::IsolateAck;
    case MessageType::Revalidate: return response == MessageType::RevalidateAck;
    case MessageType::Retire: return response == MessageType::RetireAck;
    default: return false;
  }
}

std::string_view frame_decode_status_name(FrameDecodeStatus value) noexcept {
  switch (value) {
    case FrameDecodeStatus::Ok: return "OK";
    case FrameDecodeStatus::NeedMore: return "NEED_MORE";
    case FrameDecodeStatus::InvalidMagic: return "INVALID_MAGIC";
    case FrameDecodeStatus::UnsupportedVersion: return "UNSUPPORTED_VERSION";
    case FrameDecodeStatus::OversizedPayload: return "OVERSIZED_PAYLOAD";
    case FrameDecodeStatus::CorruptIntegrity: return "CORRUPT_INTEGRITY";
    case FrameDecodeStatus::ReservedNotZero: return "RESERVED_NOT_ZERO";
    case FrameDecodeStatus::TypeOutOfDomain: return "TYPE_OUT_OF_DOMAIN";
    case FrameDecodeStatus::TrailingBytes: return "TRAILING_BYTES";
  }
  return "UNRECOGNIZED";
}

std::vector<std::byte> encode_frame(const FrameHeader& header,
                                    const std::vector<std::byte>& payload,
                                    const Limits& limits) {
  if (payload.size() > limits.max_frame_bytes ||
      payload.size() + frame_overhead_bytes > limits.max_frame_bytes) {
    return {};
  }
  if (!message_type_in_domain(header.type) || (header.flags & ~kFlagMask) != 0u ||
      header.reserved != 0u) {
    return {};
  }
  std::vector<std::byte> frame(frame_header_bytes + payload.size() + frame_trailer_bytes,
                               std::byte{0});
  std::memcpy(frame.data(), frame_magic.data(), frame_magic.size());
  frame[4] = static_cast<std::byte>((header.version >> 8) & 0xFFu);
  frame[5] = static_cast<std::byte>(header.version & 0xFFu);
  frame[6] = static_cast<std::byte>((header.type >> 8) & 0xFFu);
  frame[7] = static_cast<std::byte>(header.type & 0xFFu);
  frame[8] = static_cast<std::byte>((header.flags >> 8) & 0xFFu);
  frame[9] = static_cast<std::byte>(header.flags & 0xFFu);
  frame[10] = std::byte{0};
  frame[11] = std::byte{0};
  const auto length = static_cast<std::uint32_t>(payload.size());
  frame[12] = static_cast<std::byte>((length >> 24) & 0xFFu);
  frame[13] = static_cast<std::byte>((length >> 16) & 0xFFu);
  frame[14] = static_cast<std::byte>((length >> 8) & 0xFFu);
  frame[15] = static_cast<std::byte>(length & 0xFFu);
  for (std::size_t index = 0; index < 8; ++index) {
    frame[16 + index] = static_cast<std::byte>((header.sequence >> ((7u - index) * 8u)) & 0xFFu);
    frame[24 + index] = static_cast<std::byte>((header.epoch >> ((7u - index) * 8u)) & 0xFFu);
  }
  std::memcpy(frame.data() + 32, header.session.data(), session_handle_bytes);
  if (!payload.empty()) {
    std::memcpy(frame.data() + frame_header_bytes, payload.data(), payload.size());
  }
  const std::uint64_t crc = crc64_bytes(frame.data(), frame_header_bytes + payload.size());
  for (std::size_t index = 0; index < 8; ++index) {
    frame[frame_header_bytes + payload.size() + index] =
        static_cast<std::byte>((crc >> ((7u - index) * 8u)) & 0xFFu);
  }
  return frame;
}

namespace {

[[nodiscard]] std::uint32_t load_u32(const std::byte* data) noexcept {
  return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

[[nodiscard]] std::uint64_t load_u64(const std::byte* data) noexcept {
  return (static_cast<std::uint64_t>(load_u32(data)) << 32) |
         static_cast<std::uint64_t>(load_u32(data + 4));
}

}  // namespace

FrameDecodeResult decode_frame(const std::byte* data, std::size_t size, const Limits& limits) {
  FrameDecodeResult result;
  if (size < frame_header_bytes) {
    result.status = FrameDecodeStatus::NeedMore;
    return result;
  }
  if (std::memcmp(data, frame_magic.data(), frame_magic.size()) != 0) {
    result.status = FrameDecodeStatus::InvalidMagic;
    return result;
  }
  FrameHeader header;
  header.version = static_cast<std::uint16_t>((static_cast<unsigned>(data[4]) << 8) |
                                              static_cast<unsigned>(data[5]));
  header.type = static_cast<std::uint16_t>((static_cast<unsigned>(data[6]) << 8) |
                                           static_cast<unsigned>(data[7]));
  header.flags = static_cast<std::uint16_t>((static_cast<unsigned>(data[8]) << 8) |
                                            static_cast<unsigned>(data[9]));
  header.reserved = static_cast<std::uint16_t>((static_cast<unsigned>(data[10]) << 8) |
                                               static_cast<unsigned>(data[11]));
  header.payload_length = load_u32(data + 12);
  header.sequence = load_u64(data + 16);
  header.epoch = load_u64(data + 24);
  std::memcpy(header.session.data(), data + 32, session_handle_bytes);

  if (header.version != wire_protocol_version) {
    result.status = FrameDecodeStatus::UnsupportedVersion;
    return result;
  }
  if (header.reserved != 0u || (header.flags & ~kFlagMask) != 0u) {
    result.status = FrameDecodeStatus::ReservedNotZero;
    return result;
  }
  if (!message_type_in_domain(header.type)) {
    result.status = FrameDecodeStatus::TypeOutOfDomain;
    return result;
  }
  if (static_cast<std::size_t>(header.payload_length) + frame_overhead_bytes >
      limits.max_frame_bytes) {
    result.status = FrameDecodeStatus::OversizedPayload;
    return result;
  }
  const std::size_t total =
      frame_header_bytes + static_cast<std::size_t>(header.payload_length) + frame_trailer_bytes;
  if (size < total) {
    result.status = FrameDecodeStatus::NeedMore;
    return result;
  }
  const std::uint64_t stored = load_u64(data + frame_header_bytes + header.payload_length);
  if (stored != crc64_bytes(data, frame_header_bytes + header.payload_length)) {
    result.status = FrameDecodeStatus::CorruptIntegrity;
    return result;
  }
  result.header = header;
  result.payload.assign(data + frame_header_bytes,
                        data + frame_header_bytes + header.payload_length);
  result.consumed = total;
  result.status = FrameDecodeStatus::Ok;
  return result;
}

// ---------------------------------------------------------------------------
// Payload codecs
// ---------------------------------------------------------------------------

std::vector<std::byte> encode_hello_request(const HelloRequest& request, const Limits& limits) {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  writer.text_id(request.publisher);
  writer.binary_id(request.boot);
  writer.u64(request.expected_epoch.value());
  writer.text_id(request.provenance);
  (void)limits;
  return bytes;
}

std::optional<HelloRequest> decode_hello_request(const std::vector<std::byte>& bytes,
                                                 const Limits& limits) {
  (void)limits;
  CanonicalReader reader(bytes.data(), bytes.size());
  HelloRequest request;
  if (!reader.text_id(request.publisher) || !reader.binary_id(request.boot) ||
      !reader.counter(request.expected_epoch) || !reader.text_id(request.provenance) ||
      !reader.exhausted()) {
    return std::nullopt;
  }
  return request;
}

std::vector<std::byte> encode_hello_response(const HelloResponse& response, const Limits& limits) {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  writer.bytes(response.session.data(), response.session.size());
  writer.u64(response.epoch.value());
  writer.binary_id(response.boot);
  writer.u16(response.protocol_version);
  writer.boolean(response.accepted);
  encode_reasons(writer, response.reasons, limits);
  return bytes;
}

std::optional<HelloResponse> decode_hello_response(const std::vector<std::byte>& bytes,
                                                   const Limits& limits) {
  CanonicalReader reader(bytes.data(), bytes.size());
  HelloResponse response;
  const std::byte* session = nullptr;
  if (!reader.bytes(session_handle_bytes, session)) {
    return std::nullopt;
  }
  std::memcpy(response.session.data(), session, session_handle_bytes);
  if (!reader.counter(response.epoch) || !reader.binary_id(response.boot) ||
      !reader.u16(response.protocol_version) || !reader.boolean(response.accepted) ||
      !decode_reasons(reader, limits, response.reasons) || !reader.exhausted()) {
    return std::nullopt;
  }
  return response;
}

std::vector<std::byte> encode_operation_result(const OperationResult& result, const Limits& limits) {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  writer.u8(static_cast<std::uint8_t>(result.verdict));
  writer.text_id(result.decision);
  encode_reasons(writer, result.reasons, limits);
  return bytes;
}

std::optional<OperationResult> decode_operation_result(const std::vector<std::byte>& bytes,
                                                       const Limits& limits) {
  CanonicalReader reader(bytes.data(), bytes.size());
  OperationResult result;
  std::uint8_t verdict = 0;
  if (!reader.u8(verdict) || verdict > decision_verdict_domain_max) {
    return std::nullopt;
  }
  result.verdict = static_cast<DecisionVerdict>(verdict);
  if (!reader.text_id(result.decision) || !decode_reasons(reader, limits, result.reasons) ||
      !reader.exhausted()) {
    return std::nullopt;
  }
  return result;
}

std::vector<std::byte> encode_evidence(const ReachabilityEvidence& evidence, const Limits& limits) {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  writer.text_id(evidence.id);
  writer.u64(evidence.sequence.value());
  writer.text_id(evidence.publisher);
  writer.binary_id(evidence.publisher_boot);
  writer.u64(evidence.topology_generation.value());
  writer.u64(evidence.generation.value());
  writer.u64(evidence.observed_at_tick);
  writer.u64(evidence.validity_ticks);
  writer.u8(static_cast<std::uint8_t>(evidence.completeness));
  writer.text_id(evidence.provenance);
  const std::size_t covered =
      std::min(evidence.covered_sources.size(), limits.max_covered_components_per_evidence);
  writer.u32(static_cast<std::uint32_t>(covered));
  for (std::size_t index = 0; index < covered; ++index) {
    writer.text_id(evidence.covered_sources[index]);
  }
  const std::size_t observations =
      std::min(evidence.observations.size(), limits.max_observations_per_evidence);
  writer.u32(static_cast<std::uint32_t>(observations));
  for (std::size_t index = 0; index < observations; ++index) {
    writer.text_id(evidence.observations[index].source);
    writer.text_id(evidence.observations[index].target);
    writer.u8(static_cast<std::uint8_t>(evidence.observations[index].value));
  }
  return bytes;
}

std::optional<ReachabilityEvidence> decode_evidence(const std::vector<std::byte>& bytes,
                                                    const Limits& limits) {
  CanonicalReader reader(bytes.data(), bytes.size());
  ReachabilityEvidence evidence;
  std::uint8_t completeness = 0;
  if (!reader.text_id(evidence.id) || !reader.counter(evidence.sequence) ||
      !reader.text_id(evidence.publisher) || !reader.binary_id(evidence.publisher_boot) ||
      !reader.counter(evidence.topology_generation) || !reader.counter(evidence.generation) ||
      !reader.u64(evidence.observed_at_tick) || !reader.u64(evidence.validity_ticks) ||
      !decode_enum8(reader, evidence_completeness_domain_max, completeness) ||
      !reader.text_id(evidence.provenance)) {
    return std::nullopt;
  }
  evidence.completeness = static_cast<EvidenceCompleteness>(completeness);
  std::uint32_t covered = 0;
  if (!reader.u32(covered) ||
      static_cast<std::size_t>(covered) > limits.max_covered_components_per_evidence) {
    return std::nullopt;
  }
  evidence.covered_sources.reserve(covered);
  for (std::uint32_t index = 0; index < covered; ++index) {
    ComponentId value;
    if (!reader.text_id(value)) {
      return std::nullopt;
    }
    evidence.covered_sources.push_back(value);
  }
  std::uint32_t observations = 0;
  if (!reader.u32(observations) ||
      static_cast<std::size_t>(observations) > limits.max_observations_per_evidence) {
    return std::nullopt;
  }
  evidence.observations.reserve(observations);
  for (std::uint32_t index = 0; index < observations; ++index) {
    LinkObservation observation;
    std::uint8_t value = 0;
    if (!reader.text_id(observation.source) || !reader.text_id(observation.target) ||
        !decode_enum8(reader, reachability_domain_max, value)) {
      return std::nullopt;
    }
    observation.value = static_cast<Reachability>(value);
    evidence.observations.push_back(observation);
  }
  if (!reader.exhausted()) {
    return std::nullopt;
  }
  return evidence;
}

namespace {

void encode_partition_view(CanonicalWriter& writer, const PartitionView& view) {
  writer.text_id(view.id);
  writer.text_id(view.lineage);
  writer.u64(view.generation.value());
  writer.u64(view.member_count);
  writer.u8(static_cast<std::uint8_t>(view.certainty));
  writer.u8(static_cast<std::uint8_t>(view.lifecycle));
  writer.u8(static_cast<std::uint8_t>(view.authority_class));
  writer.u8(static_cast<std::uint8_t>(view.basis));
  writer.u8(static_cast<std::uint8_t>(view.application_state));
  writer.u32(view.authorized_bits);
  writer.u32(view.eligible_bits);
  writer.u32(view.observed_bits);
  writer.u64(view.authority_sequence);
  writer.boolean(view.authoritative);
  writer.boolean(view.pending_merge);
}

bool decode_partition_view(CanonicalReader& reader, PartitionView& view) noexcept {
  std::uint8_t certainty = 0;
  std::uint8_t lifecycle = 0;
  std::uint8_t klass = 0;
  std::uint8_t basis = 0;
  std::uint8_t state = 0;
  if (!reader.text_id(view.id) || !reader.text_id(view.lineage) ||
      !reader.counter(view.generation) || !reader.u64(view.member_count) ||
      !decode_enum8(reader, partition_certainty_domain_max, certainty) ||
      !decode_enum8(reader, partition_lifecycle_domain_max, lifecycle) ||
      !decode_enum8(reader, partition_authority_class_domain_max, klass) ||
      !decode_enum8(reader, authority_basis_domain_max, basis) ||
      !decode_enum8(reader, authority_application_state_domain_max, state) ||
      !reader.u32(view.authorized_bits) || !reader.u32(view.eligible_bits) ||
      !reader.u32(view.observed_bits) || !reader.u64(view.authority_sequence) ||
      !reader.boolean(view.authoritative) || !reader.boolean(view.pending_merge)) {
    return false;
  }
  if (!valid_capability_bits(view.authorized_bits) || !valid_capability_bits(view.eligible_bits) ||
      !valid_capability_bits(view.observed_bits)) {
    return false;
  }
  view.certainty = static_cast<PartitionCertainty>(certainty);
  view.lifecycle = static_cast<PartitionLifecycle>(lifecycle);
  view.authority_class = static_cast<PartitionAuthorityClass>(klass);
  view.basis = static_cast<AuthorityBasis>(basis);
  view.application_state = static_cast<AuthorityApplicationState>(state);
  return true;
}

}  // namespace

std::vector<std::byte> encode_assessment_view(const AssessmentView& view, const Limits& limits) {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  writer.u8(static_cast<std::uint8_t>(view.verdict));
  writer.u64(view.generation.value());
  writer.u64(view.epoch.value());
  writer.binary_id(view.boot);
  writer.boolean(view.confirmed);
  writer.boolean(view.split_brain);
  writer.u64(view.ordered_pairs);
  writer.u64(view.ordered_unknown);
  writer.u64(view.ordered_conflicting);
  writer.u64(view.ordered_stale);
  writer.u64(view.ordered_asymmetric);
  writer.u64(view.undirected_reachable);
  writer.u64(view.cross_component_indeterminate);
  writer.u64(view.topology_generation);
  writer.u64(view.reachability_generation);
  writer.text_id(view.decision);
  const std::size_t count = std::min(view.partitions.size(), limits.max_partitions);
  writer.u32(static_cast<std::uint32_t>(count));
  for (std::size_t index = 0; index < count; ++index) {
    encode_partition_view(writer, view.partitions[index]);
  }
  encode_reasons(writer, view.reasons, limits);
  return bytes;
}

std::optional<AssessmentView> decode_assessment_view(const std::vector<std::byte>& bytes,
                                                     const Limits& limits) {
  CanonicalReader reader(bytes.data(), bytes.size());
  AssessmentView view;
  std::uint8_t verdict = 0;
  if (!reader.u8(verdict) || verdict > decision_verdict_domain_max) {
    return std::nullopt;
  }
  view.verdict = static_cast<DecisionVerdict>(verdict);
  if (!reader.counter(view.generation) || !reader.counter(view.epoch) ||
      !reader.binary_id(view.boot) || !reader.boolean(view.confirmed) ||
      !reader.boolean(view.split_brain) || !reader.u64(view.ordered_pairs) ||
      !reader.u64(view.ordered_unknown) || !reader.u64(view.ordered_conflicting) ||
      !reader.u64(view.ordered_stale) || !reader.u64(view.ordered_asymmetric) ||
      !reader.u64(view.undirected_reachable) ||
      !reader.u64(view.cross_component_indeterminate) || !reader.u64(view.topology_generation) ||
      !reader.u64(view.reachability_generation) || !reader.text_id(view.decision)) {
    return std::nullopt;
  }
  std::uint32_t count = 0;
  if (!reader.u32(count) || static_cast<std::size_t>(count) > limits.max_partitions) {
    return std::nullopt;
  }
  view.partitions.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    PartitionView partition;
    if (!decode_partition_view(reader, partition)) {
      return std::nullopt;
    }
    view.partitions.push_back(partition);
  }
  if (!decode_reasons(reader, limits, view.reasons) || !reader.exhausted()) {
    return std::nullopt;
  }
  return view;
}

std::vector<std::byte> encode_status_view(const StatusView& view) noexcept {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  writer.u64(view.epoch.value());
  writer.binary_id(view.boot);
  writer.binary_id(view.incarnation);
  writer.u64(view.tick);
  writer.u64(view.partition_generation);
  writer.u64(view.authority_sequence);
  writer.u64(view.decision_sequence);
  writer.u64(view.attempt_sequence);
  writer.u64(view.lineage_sequence);
  writer.u64(view.fence_sequence);
  writer.u64(view.partition_count);
  writer.u64(view.lineage_records);
  writer.u64(view.fence_records);
  writer.u64(view.evidence_bundles);
  writer.u64(view.interrupted_authorities);
  writer.u64(view.decision_history);
  writer.u64(view.decisions_dropped);
  writer.boolean(view.durable);
  return bytes;
}

std::optional<StatusView> decode_status_view(const std::vector<std::byte>& bytes,
                                             const Limits& limits) noexcept {
  (void)limits;
  CanonicalReader reader(bytes.data(), bytes.size());
  StatusView view;
  if (!reader.counter(view.epoch) || !reader.binary_id(view.boot) ||
      !reader.binary_id(view.incarnation) || !reader.u64(view.tick) ||
      !reader.u64(view.partition_generation) || !reader.u64(view.authority_sequence) ||
      !reader.u64(view.decision_sequence) || !reader.u64(view.attempt_sequence) ||
      !reader.u64(view.lineage_sequence) || !reader.u64(view.fence_sequence) ||
      !reader.u64(view.partition_count) || !reader.u64(view.lineage_records) ||
      !reader.u64(view.fence_records) || !reader.u64(view.evidence_bundles) ||
      !reader.u64(view.interrupted_authorities) || !reader.u64(view.decision_history) ||
      !reader.u64(view.decisions_dropped) || !reader.boolean(view.durable) ||
      !reader.exhausted()) {
    return std::nullopt;
  }
  return view;
}

AssessmentView make_assessment_view(const PartitionAssessment& assessment,
                                    const RuntimeStatus& status) {
  AssessmentView view;
  view.verdict = assessment.decision.verdict;
  view.generation = assessment.generation;
  view.epoch = status.epoch;
  view.boot = status.boot;
  view.confirmed = assessment.confirmed;
  view.split_brain = assessment.split_brain;
  view.ordered_pairs = assessment.reachability.counters.ordered_pairs;
  view.ordered_unknown = assessment.reachability.counters.ordered_unknown;
  view.ordered_conflicting = assessment.reachability.counters.ordered_conflicting;
  view.ordered_stale = assessment.reachability.counters.ordered_stale;
  view.ordered_asymmetric = assessment.reachability.counters.ordered_asymmetric;
  view.undirected_reachable = assessment.reachability.counters.undirected_reachable;
  view.cross_component_indeterminate =
      assessment.reachability.counters.cross_component_indeterminate;
  view.topology_generation = assessment.reachability.topology_generation.value();
  view.reachability_generation = assessment.reachability.reachability_generation.value();
  view.decision = assessment.decision.id;
  view.reasons = assessment.decision.reasons;
  view.partitions.reserve(assessment.partitions.size());
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
  return view;
}

StatusView make_status_view(const RuntimeStatus& status) {
  StatusView view;
  view.epoch = status.epoch;
  view.boot = status.boot;
  view.incarnation = status.incarnation;
  view.tick = status.tick;
  view.partition_generation = status.partition_generation.value();
  view.authority_sequence = status.authority_sequence.value();
  view.decision_sequence = status.decision_sequence.value();
  view.attempt_sequence = status.attempt_sequence.value();
  view.lineage_sequence = status.lineage_sequence.value();
  view.fence_sequence = status.fence_sequence.value();
  view.partition_count = status.partition_count;
  view.lineage_records = status.lineage_records;
  view.fence_records = status.fence_records;
  view.evidence_bundles = status.evidence_bundles;
  view.interrupted_authorities = status.interrupted_authorities;
  view.decision_history = status.decision_history;
  view.decisions_dropped = status.decisions_dropped;
  view.durable = status.durable;
  return view;
}

}  // namespace fabric_partition_manager
