// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Bounded framed transport.
//
// Every frame carries a magic and version, a message type, a declared payload
// length, a monotonic per-session sequence, the coordinator epoch the sender
// believes is current, an opaque session handle established by the handshake,
// and a CRC-64/XZ integrity field over the header and payload.
//
// Decoding is total and sticky-failure. A declared payload length is refused
// before any allocation, every enum is validated against its domain, truncated
// prefixes are distinguished from corrupt frames, and trailing bytes after a
// declared payload are an error rather than something to ignore.
//
// Trust boundary: this protocol is NOT authenticated and NOT encrypted. The
// integrity field detects corruption, not a hostile peer. Reaching the port is
// enough to send a well-formed frame. Session binding prevents one session from
// acting under another session's handle, epoch or boot identity, and nothing
// more is claimed.
#ifndef FABRIC_PARTITION_MANAGER_PROTOCOL_HPP
#define FABRIC_PARTITION_MANAGER_PROTOCOL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fabric_partition_manager/decision.hpp"
#include "fabric_partition_manager/evidence.hpp"
#include "fabric_partition_manager/limits.hpp"
#include "fabric_partition_manager/runtime.hpp"

namespace fabric_partition_manager {

inline constexpr std::array<char, 4> frame_magic = {'F', 'P', 'M', '1'};
inline constexpr std::size_t frame_header_bytes = 48;
inline constexpr std::size_t frame_trailer_bytes = 8;
inline constexpr std::size_t frame_overhead_bytes = frame_header_bytes + frame_trailer_bytes;
inline constexpr std::size_t session_handle_bytes = 16;

enum class MessageType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  Goodbye = 3,
  ProtocolError = 4,
  PublishEvidence = 10,
  PublishEvidenceAck = 11,
  AdoptTopology = 12,
  AdoptTopologyAck = 13,
  SetPolicy = 14,
  SetPolicyAck = 15,
  Assess = 20,
  AssessAck = 21,
  QueryStatus = 30,
  QueryStatusAck = 31,
  QueryPartitions = 32,
  QueryPartitionsAck = 33,
  QueryFences = 34,
  QueryFencesAck = 35,
  QueryLineage = 36,
  QueryLineageAck = 37,
  Merge = 40,
  MergeAck = 41,
  Isolate = 42,
  IsolateAck = 43,
  Revalidate = 44,
  RevalidateAck = 45,
  Retire = 46,
  RetireAck = 47,
};

inline constexpr std::uint16_t message_type_domain_max = 47;

[[nodiscard]] std::string_view message_type_name(MessageType value) noexcept;
[[nodiscard]] bool message_type_in_domain(std::uint16_t value) noexcept;
// True when the message is a response to the given request.
[[nodiscard]] bool message_is_response_to(MessageType response, MessageType request) noexcept;

struct FrameHeader {
  std::uint16_t version = 0;
  std::uint16_t type = 0;
  std::uint16_t flags = 0;
  std::uint16_t reserved = 0;
  std::uint32_t payload_length = 0;
  std::uint64_t sequence = 0;
  std::uint64_t epoch = 0;
  std::array<std::byte, session_handle_bytes> session{};
};

enum class FrameDecodeStatus : std::uint8_t {
  Ok = 0,
  NeedMore = 1,
  InvalidMagic = 2,
  UnsupportedVersion = 3,
  OversizedPayload = 4,
  CorruptIntegrity = 5,
  ReservedNotZero = 6,
  TypeOutOfDomain = 7,
  TrailingBytes = 8,
};

[[nodiscard]] std::string_view frame_decode_status_name(FrameDecodeStatus value) noexcept;

struct FrameDecodeResult {
  FrameDecodeStatus status = FrameDecodeStatus::NeedMore;
  FrameHeader header;
  std::vector<std::byte> payload;
  std::size_t consumed = 0;
};

// Encodes a frame. Returns an empty vector when the payload exceeds the bound.
[[nodiscard]] std::vector<std::byte> encode_frame(const FrameHeader& header,
                                                  const std::vector<std::byte>& payload,
                                                  const Limits& limits);

// Decodes exactly one frame from the front of the buffer. Never allocates on
// behalf of an unvalidated declared length.
[[nodiscard]] FrameDecodeResult decode_frame(const std::byte* data, std::size_t size,
                                             const Limits& limits);

// ---------------------------------------------------------------------------
// Message payloads
// ---------------------------------------------------------------------------

struct HelloRequest {
  PublisherId publisher;
  PublisherBootId boot;
  CoordinatorEpoch expected_epoch;
  Provenance provenance;
};

struct HelloResponse {
  std::array<std::byte, session_handle_bytes> session{};
  CoordinatorEpoch epoch;
  CoordinatorBootId boot;
  std::uint16_t protocol_version = 0;
  bool accepted = false;
  std::vector<Reason> reasons;
};

// The compact, wire-level description of a partition. The durable and
// in-process representations are richer; the wire form carries exactly what a
// remote operator needs to act.
struct PartitionView {
  PartitionId id;
  LineageId lineage;
  PartitionGeneration generation;
  std::uint64_t member_count = 0;
  PartitionCertainty certainty = PartitionCertainty::Partial;
  PartitionLifecycle lifecycle = PartitionLifecycle::Discovered;
  PartitionAuthorityClass authority_class = PartitionAuthorityClass::Unassigned;
  AuthorityBasis basis = AuthorityBasis::None;
  AuthorityApplicationState application_state = AuthorityApplicationState::NotApplicable;
  std::uint32_t authorized_bits = 0;
  std::uint32_t eligible_bits = 0;
  std::uint32_t observed_bits = 0;
  std::uint64_t authority_sequence = 0;
  bool authoritative = false;
  bool pending_merge = false;
};

struct AssessmentView {
  DecisionVerdict verdict = DecisionVerdict::Invalid;
  PartitionGeneration generation;
  CoordinatorEpoch epoch;
  CoordinatorBootId boot;
  bool confirmed = false;
  bool split_brain = false;
  std::uint64_t ordered_pairs = 0;
  std::uint64_t ordered_unknown = 0;
  std::uint64_t ordered_conflicting = 0;
  std::uint64_t ordered_stale = 0;
  std::uint64_t ordered_asymmetric = 0;
  std::uint64_t undirected_reachable = 0;
  std::uint64_t cross_component_indeterminate = 0;
  std::uint64_t topology_generation = 0;
  std::uint64_t reachability_generation = 0;
  DecisionId decision;
  std::vector<PartitionView> partitions;
  std::vector<Reason> reasons;
};

struct StatusView {
  CoordinatorEpoch epoch;
  CoordinatorBootId boot;
  ProcessIncarnationId incarnation;
  std::uint64_t tick = 0;
  std::uint64_t partition_generation = 0;
  std::uint64_t authority_sequence = 0;
  std::uint64_t decision_sequence = 0;
  std::uint64_t attempt_sequence = 0;
  std::uint64_t lineage_sequence = 0;
  std::uint64_t fence_sequence = 0;
  std::uint64_t partition_count = 0;
  std::uint64_t lineage_records = 0;
  std::uint64_t fence_records = 0;
  std::uint64_t evidence_bundles = 0;
  std::uint64_t interrupted_authorities = 0;
  std::uint64_t decision_history = 0;
  std::uint64_t decisions_dropped = 0;
  bool durable = false;
};

// A uniform response envelope: the verdict plus the reason codes that explain
// it, so a refusal is never an empty reply.
struct OperationResult {
  DecisionVerdict verdict = DecisionVerdict::Invalid;
  DecisionId decision;
  std::vector<Reason> reasons;
};

[[nodiscard]] std::vector<std::byte> encode_hello_request(const HelloRequest& request,
                                                          const Limits& limits);
[[nodiscard]] std::optional<HelloRequest> decode_hello_request(const std::vector<std::byte>& bytes,
                                                               const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_hello_response(const HelloResponse& response,
                                                           const Limits& limits);
[[nodiscard]] std::optional<HelloResponse> decode_hello_response(
    const std::vector<std::byte>& bytes, const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_operation_result(const OperationResult& result,
                                                             const Limits& limits);
[[nodiscard]] std::optional<OperationResult> decode_operation_result(
    const std::vector<std::byte>& bytes, const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_evidence(const ReachabilityEvidence& evidence,
                                                     const Limits& limits);
[[nodiscard]] std::optional<ReachabilityEvidence> decode_evidence(
    const std::vector<std::byte>& bytes, const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_assessment_view(const AssessmentView& view,
                                                            const Limits& limits);
[[nodiscard]] std::optional<AssessmentView> decode_assessment_view(
    const std::vector<std::byte>& bytes, const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_status_view(const StatusView& view) noexcept;
[[nodiscard]] std::optional<StatusView> decode_status_view(const std::vector<std::byte>& bytes,
                                                           const Limits& limits) noexcept;

[[nodiscard]] AssessmentView make_assessment_view(const PartitionAssessment& assessment,
                                                  const RuntimeStatus& status);
[[nodiscard]] StatusView make_status_view(const RuntimeStatus& status);

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_PROTOCOL_HPP
