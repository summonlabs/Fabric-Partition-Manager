// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/persistence.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "fabric_partition_manager/encoding.hpp"
#include "fabric_partition_manager/version.hpp"
#include "platform.hpp"

#if defined(_WIN32)
#include <io.h>
#endif

namespace fabric_partition_manager {
namespace {

std::FILE* open_file(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
  std::wstring wide_mode;
  for (const char* cursor = mode; *cursor != '\0'; ++cursor) {
    wide_mode.push_back(static_cast<wchar_t>(*cursor));
  }
  std::FILE* file = nullptr;
  if (::_wfopen_s(&file, path.wstring().c_str(), wide_mode.c_str()) != 0) {
    return nullptr;
  }
  return file;
#else
  return std::fopen(path.string().c_str(), mode);
#endif
}

bool sync_file(std::FILE* file) {
#if defined(_WIN32)
  return ::_commit(::_fileno(file)) == 0;
#else
  return ::fsync(::fileno(file)) == 0;
#endif
}

bool truncate_file(std::FILE* file, std::uint64_t size) {
#if defined(_WIN32)
  return ::_chsize_s(::_fileno(file), static_cast<__int64>(size)) == 0;
#else
  return ::ftruncate(::fileno(file), static_cast<off_t>(size)) == 0;
#endif
}

[[nodiscard]] std::uint32_t read_u32(const std::byte* data) noexcept {
  return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

[[nodiscard]] std::uint64_t read_u64(const std::byte* data) noexcept {
  return (static_cast<std::uint64_t>(read_u32(data)) << 32) |
         static_cast<std::uint64_t>(read_u32(data + 4));
}

void write_u32(std::byte* data, std::uint32_t value) noexcept {
  data[0] = static_cast<std::byte>((value >> 24) & 0xFFu);
  data[1] = static_cast<std::byte>((value >> 16) & 0xFFu);
  data[2] = static_cast<std::byte>((value >> 8) & 0xFFu);
  data[3] = static_cast<std::byte>(value & 0xFFu);
}

void write_u64(std::byte* data, std::uint64_t value) noexcept {
  write_u32(data, static_cast<std::uint32_t>((value >> 32) & 0xFFFF'FFFFu));
  write_u32(data + 4, static_cast<std::uint32_t>(value & 0xFFFF'FFFFu));
}

[[nodiscard]] std::vector<std::byte> make_header(std::string_view magic) {
  std::vector<std::byte> header(store_header_bytes, std::byte{0});
  std::memcpy(header.data(), magic.data(), magic.size());
  write_u32(header.data() + 8, durable_format_version);
  write_u32(header.data() + 12, 0);
  write_u64(header.data() + 16, crc64_bytes(header.data(), 16));
  return header;
}

[[nodiscard]] bool validate_header(const std::byte* data, std::size_t size, std::string_view magic,
                                   ReasonCode& code) noexcept {
  if (size < store_header_bytes) {
    code = ReasonCode::StoreIntegrityFailure;
    return false;
  }
  if (std::memcmp(data, magic.data(), magic.size()) != 0) {
    code = ReasonCode::StoreIntegrityFailure;
    return false;
  }
  if (read_u32(data + 8) != durable_format_version) {
    code = ReasonCode::StoreVersionUnsupported;
    return false;
  }
  if (read_u64(data + 16) != crc64_bytes(data, 16)) {
    code = ReasonCode::StoreIntegrityFailure;
    return false;
  }
  code = ReasonCode::Ok;
  return true;
}

[[nodiscard]] std::vector<std::byte> encode_record(const DurableRecord& record) {
  std::vector<std::byte> bytes(record_header_bytes + record.payload.size() + record_trailer_bytes,
                               std::byte{0});
  const auto type = static_cast<std::uint16_t>(record.type);
  bytes[0] = static_cast<std::byte>((type >> 8) & 0xFFu);
  bytes[1] = static_cast<std::byte>(type & 0xFFu);
  bytes[2] = std::byte{0};
  bytes[3] = std::byte{0};
  write_u32(bytes.data() + 4, static_cast<std::uint32_t>(record.payload.size()));
  write_u64(bytes.data() + 8, record.sequence);
  if (!record.payload.empty()) {
    std::memcpy(bytes.data() + record_header_bytes, record.payload.data(), record.payload.size());
  }
  write_u64(bytes.data() + record_header_bytes + record.payload.size(),
            crc64_bytes(bytes.data(), record_header_bytes + record.payload.size()));
  return bytes;
}

enum class RecordParseStatus : std::uint8_t { Ok = 0, Short = 1, Invalid = 2, Integrity = 3 };

// Parses one record from a complete byte image. A declared length is validated
// against the configured bound before anything is read from it.
RecordParseStatus parse_record(const std::byte* data, std::size_t size, std::size_t max_record_bytes,
                               DurableRecord& out, std::size_t& consumed) noexcept {
  if (size < record_header_bytes) {
    return RecordParseStatus::Short;
  }
  const auto type = static_cast<std::uint16_t>((static_cast<unsigned>(data[0]) << 8) |
                                               static_cast<unsigned>(data[1]));
  if (type == 0 || type > durable_record_type_domain_max) {
    return RecordParseStatus::Invalid;
  }
  const std::uint32_t payload_size = read_u32(data + 4);
  if (static_cast<std::size_t>(payload_size) > max_record_bytes) {
    return RecordParseStatus::Invalid;
  }
  const std::size_t total =
      record_header_bytes + static_cast<std::size_t>(payload_size) + record_trailer_bytes;
  if (size < total) {
    return RecordParseStatus::Short;
  }
  if (read_u64(data + record_header_bytes + payload_size) !=
      crc64_bytes(data, record_header_bytes + payload_size)) {
    return RecordParseStatus::Integrity;
  }
  out.type = static_cast<DurableRecordType>(type);
  out.sequence = read_u64(data + 8);
  out.payload.assign(data + record_header_bytes, data + record_header_bytes + payload_size);
  consumed = total;
  return RecordParseStatus::Ok;
}

// ---------------------------------------------------------------------------
// Shared field codecs
// ---------------------------------------------------------------------------

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

[[nodiscard]] bool valid_capability_bits(std::uint32_t bits) noexcept {
  return (bits & ~((1u << authority_capability_count) - 1u)) == 0u;
}

[[nodiscard]] bool decode_capability(CanonicalReader& reader, CapabilitySet& out) noexcept {
  std::uint32_t bits = 0;
  if (!reader.u32(bits) || !valid_capability_bits(bits)) {
    return false;
  }
  out = CapabilitySet(bits);
  return true;
}

void encode_members(CanonicalWriter& writer, const std::vector<ComponentId>& members) {
  writer.u32(static_cast<std::uint32_t>(members.size()));
  for (const ComponentId& member : members) {
    writer.text_id(member);
  }
}

bool decode_members(CanonicalReader& reader, const Limits& limits,
                    std::vector<ComponentId>& out) noexcept {
  std::uint32_t count = 0;
  if (!reader.u32(count) || static_cast<std::size_t>(count) > limits.max_components) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    ComponentId member;
    if (!reader.text_id(member)) {
      return false;
    }
    if (index > 0 && !(out.back() < member)) {
      return false;
    }
    out.push_back(member);
  }
  return true;
}

}  // namespace

std::string_view durable_record_type_name(DurableRecordType value) noexcept {
  switch (value) {
    case DurableRecordType::EpochState: return "EPOCH_STATE";
    case DurableRecordType::PolicyDefinition: return "POLICY_DEFINITION";
    case DurableRecordType::TopologyDefinition: return "TOPOLOGY_DEFINITION";
    case DurableRecordType::LineageRecord: return "LINEAGE_RECORD";
    case DurableRecordType::DecisionRecord: return "DECISION_RECORD";
    case DurableRecordType::FenceRecord: return "FENCE_RECORD";
  }
  return "UNRECOGNIZED";
}

// ---------------------------------------------------------------------------
// Codecs
// ---------------------------------------------------------------------------

std::vector<std::byte> encode_authority(const AuthorityVector& authority) noexcept {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  writer.u8(static_cast<std::uint8_t>(authority.authority_class));
  writer.u8(static_cast<std::uint8_t>(authority.basis));
  writer.u8(static_cast<std::uint8_t>(authority.application_state));
  writer.u32(authority.observed.bits());
  writer.u32(authority.eligible.bits());
  writer.u32(authority.recommended.bits());
  writer.u32(authority.authorized.bits());
  writer.u32(authority.withheld.bits());
  writer.u32(authority.denied.bits());
  writer.boolean(authority.revocable);
  encode_reasons(writer, authority.reasons, default_limits());
  return bytes;
}

std::optional<AuthorityVector> decode_authority(const std::vector<std::byte>& bytes,
                                                std::size_t& offset,
                                                const Limits& limits) noexcept {
  if (offset > bytes.size()) {
    return std::nullopt;
  }
  CanonicalReader reader(bytes.data() + offset, bytes.size() - offset);
  std::uint8_t klass = 0;
  std::uint8_t basis = 0;
  std::uint8_t state = 0;
  if (!reader.u8(klass) || !reader.u8(basis) || !reader.u8(state)) {
    return std::nullopt;
  }
  if (klass > partition_authority_class_domain_max || basis > authority_basis_domain_max ||
      state > authority_application_state_domain_max) {
    return std::nullopt;
  }
  AuthorityVector authority;
  authority.authority_class = static_cast<PartitionAuthorityClass>(klass);
  authority.basis = static_cast<AuthorityBasis>(basis);
  authority.application_state = static_cast<AuthorityApplicationState>(state);
  if (!decode_capability(reader, authority.observed) ||
      !decode_capability(reader, authority.eligible) ||
      !decode_capability(reader, authority.recommended) ||
      !decode_capability(reader, authority.authorized) ||
      !decode_capability(reader, authority.withheld) ||
      !decode_capability(reader, authority.denied)) {
    return std::nullopt;
  }
  if (!reader.boolean(authority.revocable)) {
    return std::nullopt;
  }
  if (!decode_reasons(reader, limits, authority.reasons)) {
    return std::nullopt;
  }
  offset += reader.offset();
  return authority;
}

std::vector<std::byte> encode_epoch_state(const EpochState& state) noexcept {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  writer.u64(state.epoch.value());
  writer.binary_id(state.boot);
  writer.binary_id(state.incarnation);
  writer.u64(state.boot_sequence.value());
  writer.u64(state.partition_generation.value());
  writer.u64(state.authority_sequence.value());
  writer.u64(state.decision_sequence.value());
  writer.u64(state.attempt_sequence.value());
  writer.u64(state.lineage_sequence.value());
  writer.u64(state.fence_sequence.value());
  writer.u64(state.tick);
  writer.u32(static_cast<std::uint32_t>(state.live_grants.size()));
  for (const AuthorityGrantMark& mark : state.live_grants) {
    writer.text_id(mark.partition);
    writer.text_id(mark.lineage);
    writer.u64(mark.authority_sequence.value());
  }
  return bytes;
}

std::optional<EpochState> decode_epoch_state(const std::vector<std::byte>& bytes,
                                             const Limits& limits) noexcept {
  CanonicalReader reader(bytes.data(), bytes.size());
  EpochState state;
  if (!reader.counter(state.epoch) || !reader.binary_id(state.boot) ||
      !reader.binary_id(state.incarnation) || !reader.counter(state.boot_sequence) ||
      !reader.counter(state.partition_generation) || !reader.counter(state.authority_sequence) ||
      !reader.counter(state.decision_sequence) || !reader.counter(state.attempt_sequence) ||
      !reader.counter(state.lineage_sequence) || !reader.counter(state.fence_sequence) ||
      !reader.u64(state.tick)) {
    return std::nullopt;
  }
  std::uint32_t grant_count = 0;
  if (!reader.u32(grant_count) || static_cast<std::size_t>(grant_count) > limits.max_fence_records) {
    return std::nullopt;
  }
  state.live_grants.reserve(grant_count);
  for (std::uint32_t index = 0; index < grant_count; ++index) {
    AuthorityGrantMark mark;
    if (!reader.text_id(mark.partition) || !reader.text_id(mark.lineage) ||
        !reader.counter(mark.authority_sequence)) {
      return std::nullopt;
    }
    state.live_grants.push_back(mark);
  }
  if (!reader.exhausted()) {
    return std::nullopt;
  }
  return state;
}

std::vector<std::byte> encode_policy(const PartitionPolicy& policy) noexcept {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  writer.text_id(policy.id);
  writer.u64(policy.generation.value());
  writer.u16(policy.schema_version);
  writer.u32(static_cast<std::uint32_t>(policy.weights.size()));
  for (const ComponentWeight& entry : policy.weights) {
    writer.text_id(entry.component);
    writer.u64(entry.weight);
    writer.boolean(entry.voter);
  }
  const auto encode_quorum = [&writer](const QuorumRequirement& quorum) {
    writer.u32(quorum.min_components);
    writer.u32(quorum.min_voters);
    writer.u64(quorum.min_weight);
    writer.u64(quorum.domain_weight);
    writer.boolean(quorum.require_strict_majority);
  };
  encode_quorum(policy.full_authority);
  encode_quorum(policy.degraded_authority);
  writer.u32(policy.degraded_capabilities.bits());
  writer.u32(policy.readonly_capabilities.bits());
  writer.u64(policy.max_evidence_age_ticks);
  writer.boolean(policy.require_symmetric_reachability);
  writer.boolean(policy.require_confirmed_partition_for_write);
  writer.boolean(policy.require_confirmed_partition_for_read);
  writer.boolean(policy.allow_multiple_authoritative_partitions);
  writer.boolean(policy.unknown_evidence_downgrades);
  return bytes;
}

std::optional<PartitionPolicy> decode_policy(const std::vector<std::byte>& bytes,
                                             const Limits& limits) noexcept {
  CanonicalReader reader(bytes.data(), bytes.size());
  PartitionPolicy policy;
  std::uint32_t weight_count = 0;
  if (!reader.text_id(policy.id) || !reader.counter(policy.generation) ||
      !reader.u16(policy.schema_version)) {
    return std::nullopt;
  }
  if (policy.schema_version != policy_schema_version) {
    return std::nullopt;
  }
  if (!reader.u32(weight_count) || static_cast<std::size_t>(weight_count) > limits.max_components) {
    return std::nullopt;
  }
  policy.weights.reserve(weight_count);
  for (std::uint32_t index = 0; index < weight_count; ++index) {
    ComponentWeight entry;
    if (!reader.text_id(entry.component) || !reader.u64(entry.weight) ||
        !reader.boolean(entry.voter)) {
      return std::nullopt;
    }
    if (index > 0 && !(policy.weights.back().component < entry.component)) {
      return std::nullopt;
    }
    policy.weights.push_back(entry);
  }
  const auto decode_quorum = [&reader](QuorumRequirement& quorum) -> bool {
    return reader.u32(quorum.min_components) && reader.u32(quorum.min_voters) &&
           reader.u64(quorum.min_weight) && reader.u64(quorum.domain_weight) &&
           reader.boolean(quorum.require_strict_majority);
  };
  if (!decode_quorum(policy.full_authority) || !decode_quorum(policy.degraded_authority)) {
    return std::nullopt;
  }
  std::uint32_t degraded_bits = 0;
  std::uint32_t readonly_bits = 0;
  if (!reader.u32(degraded_bits) || !reader.u32(readonly_bits) ||
      !valid_capability_bits(degraded_bits) || !valid_capability_bits(readonly_bits)) {
    return std::nullopt;
  }
  policy.degraded_capabilities = CapabilitySet(degraded_bits);
  policy.readonly_capabilities = CapabilitySet(readonly_bits);
  if (!reader.u64(policy.max_evidence_age_ticks) ||
      !reader.boolean(policy.require_symmetric_reachability) ||
      !reader.boolean(policy.require_confirmed_partition_for_write) ||
      !reader.boolean(policy.require_confirmed_partition_for_read) ||
      !reader.boolean(policy.allow_multiple_authoritative_partitions) ||
      !reader.boolean(policy.unknown_evidence_downgrades) || !reader.exhausted()) {
    return std::nullopt;
  }
  return policy;
}

std::vector<std::byte> encode_topology(const TopologyDefinition& definition) noexcept {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  writer.text_id(definition.id);
  writer.u64(definition.generation.value());
  writer.text_id(definition.provenance);
  encode_members(writer, definition.components);
  return bytes;
}

std::optional<TopologyDefinition> decode_topology(const std::vector<std::byte>& bytes,
                                                  const Limits& limits) noexcept {
  CanonicalReader reader(bytes.data(), bytes.size());
  TopologyDefinition definition;
  if (!reader.text_id(definition.id) || !reader.counter(definition.generation) ||
      !reader.text_id(definition.provenance) || !decode_members(reader, limits, definition.components) ||
      !reader.exhausted()) {
    return std::nullopt;
  }
  return definition;
}

std::vector<std::byte> encode_lineage(const LineageRecord& record) noexcept {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  writer.u64(record.sequence.value());
  writer.text_id(record.lineage);
  writer.u64(record.generation.value());
  writer.digest(record.membership.value());
  encode_members(writer, record.members);
  writer.u8(static_cast<std::uint8_t>(record.event));
  writer.text_id(record.parent_left);
  writer.text_id(record.parent_right);
  writer.u64(record.epoch.value());
  writer.binary_id(record.boot);
  writer.text_id(record.decision);
  writer.u64(record.tick);
  writer.digest(record.digest.value());
  return bytes;
}

std::optional<LineageRecord> decode_lineage(const std::vector<std::byte>& bytes,
                                            const Limits& limits) noexcept {
  CanonicalReader reader(bytes.data(), bytes.size());
  LineageRecord record;
  std::uint8_t event = 0;
  if (!reader.counter(record.sequence) || !reader.text_id(record.lineage) ||
      !reader.counter(record.generation) || !reader.typed_digest(record.membership) ||
      !decode_members(reader, limits, record.members) || !reader.u8(event) ||
      event > lineage_event_kind_domain_max) {
    return std::nullopt;
  }
  record.event = static_cast<LineageEventKind>(event);
  if (!reader.text_id(record.parent_left) || !reader.text_id(record.parent_right) ||
      !reader.counter(record.epoch) || !reader.binary_id(record.boot) ||
      !reader.text_id(record.decision) || !reader.u64(record.tick) ||
      !reader.typed_digest(record.digest) || !reader.exhausted()) {
    return std::nullopt;
  }
  if (!(compute_membership_digest(record.members) == record.membership)) {
    return std::nullopt;
  }
  if (!(compute_lineage_digest(record) == record.digest)) {
    return std::nullopt;
  }
  return record;
}

std::vector<std::byte> encode_decision(const Decision& decision) noexcept {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  writer.text_id(decision.id);
  writer.u8(static_cast<std::uint8_t>(decision.kind));
  writer.u8(static_cast<std::uint8_t>(decision.verdict));
  writer.u8(static_cast<std::uint8_t>(decision.scope.kind));
  writer.text_id(decision.scope.id);
  writer.u64(decision.bindings.epoch.value());
  writer.binary_id(decision.bindings.boot);
  writer.binary_id(decision.bindings.incarnation);
  writer.u64(decision.bindings.topology_generation.value());
  writer.digest(decision.bindings.topology_digest.value());
  writer.u64(decision.bindings.reachability_generation.value());
  writer.digest(decision.bindings.evidence_digest.value());

  writer.u64(decision.bindings.policy_generation.value());
  writer.u64(decision.bindings.partition_generation.value());
  writer.u64(decision.bindings.authority_sequence.value());
  writer.u64(decision.bindings.decision_sequence.value());
  writer.text_id(decision.bindings.attempt);
  writer.u64(decision.bindings.evaluated_at_tick);
  writer.u32(static_cast<std::uint32_t>(decision.subjects.size()));
  for (const PartitionId& subject : decision.subjects) {
    writer.text_id(subject);
  }
  const std::vector<std::byte> authority = encode_authority(decision.authority);
  writer.bytes(authority.data(), authority.size());
  writer.u32(static_cast<std::uint32_t>(decision.fences_issued.size()));
  for (const FenceId& fence : decision.fences_issued) {
    writer.text_id(fence);
  }
  writer.u32(static_cast<std::uint32_t>(decision.fenced_partitions.size()));
  for (const PartitionId& partition : decision.fenced_partitions) {
    writer.text_id(partition);
  }
  writer.u32(static_cast<std::uint32_t>(decision.revocation_triggers.size()));
  for (const ReasonCode code : decision.revocation_triggers) {
    writer.u16(static_cast<std::uint16_t>(code));
  }
  writer.boolean(decision.revocable);
  encode_reasons(writer, decision.reasons, default_limits());
  return bytes;
}

std::optional<Decision> decode_decision(const std::vector<std::byte>& bytes,
                                        const Limits& limits) noexcept {
  CanonicalReader reader(bytes.data(), bytes.size());
  Decision decision;
  std::uint8_t kind = 0;
  std::uint8_t verdict = 0;
  std::uint8_t scope = 0;
  if (!reader.text_id(decision.id) || !reader.u8(kind) || !reader.u8(verdict) ||
      !reader.u8(scope) || !reader.text_id(decision.scope.id)) {
    return std::nullopt;
  }
  if (kind > decision_kind_domain_max || verdict > decision_verdict_domain_max ||
      scope > scope_kind_domain_max) {
    return std::nullopt;
  }
  decision.kind = static_cast<DecisionKind>(kind);
  decision.verdict = static_cast<DecisionVerdict>(verdict);
  decision.scope.kind = static_cast<ScopeKind>(scope);
  if (!reader.counter(decision.bindings.epoch) || !reader.binary_id(decision.bindings.boot) ||
      !reader.binary_id(decision.bindings.incarnation) ||
      !reader.counter(decision.bindings.topology_generation) ||
      !reader.typed_digest(decision.bindings.topology_digest) ||
      !reader.counter(decision.bindings.reachability_generation) ||
      !reader.typed_digest(decision.bindings.evidence_digest) ||
      !reader.counter(decision.bindings.policy_generation) ||
      !reader.counter(decision.bindings.partition_generation) ||
      !reader.counter(decision.bindings.authority_sequence) ||
      !reader.counter(decision.bindings.decision_sequence) ||
      !reader.text_id(decision.bindings.attempt) ||
      !reader.u64(decision.bindings.evaluated_at_tick)) {
    return std::nullopt;
  }
  std::uint32_t subject_count = 0;
  if (!reader.u32(subject_count) || subject_count > limits.max_partitions) {
    return std::nullopt;
  }
  decision.subjects.reserve(subject_count);
  for (std::uint32_t index = 0; index < subject_count; ++index) {
    PartitionId subject;
    if (!reader.text_id(subject)) {
      return std::nullopt;
    }
    decision.subjects.push_back(subject);
  }
  std::size_t authority_offset = reader.offset();
  const auto authority = decode_authority(bytes, authority_offset, limits);
  if (!authority.has_value() || authority_offset > bytes.size()) {
    return std::nullopt;
  }
  decision.authority = *authority;
  CanonicalReader tail(bytes.data() + authority_offset, bytes.size() - authority_offset);
  std::uint32_t fence_count = 0;
  if (!tail.u32(fence_count) || fence_count > limits.max_fence_records) {
    return std::nullopt;
  }
  decision.fences_issued.reserve(fence_count);
  for (std::uint32_t index = 0; index < fence_count; ++index) {
    FenceId fence;
    if (!tail.text_id(fence)) {
      return std::nullopt;
    }
    decision.fences_issued.push_back(fence);
  }
  std::uint32_t fenced_count = 0;
  if (!tail.u32(fenced_count) || fenced_count > limits.max_partitions) {
    return std::nullopt;
  }
  decision.fenced_partitions.reserve(fenced_count);
  for (std::uint32_t index = 0; index < fenced_count; ++index) {
    PartitionId partition;
    if (!tail.text_id(partition)) {
      return std::nullopt;
    }
    decision.fenced_partitions.push_back(partition);
  }
  std::uint32_t trigger_count = 0;
  if (!tail.u32(trigger_count) || trigger_count > limits.max_reasons_per_decision) {
    return std::nullopt;
  }
  decision.revocation_triggers.reserve(trigger_count);
  for (std::uint32_t index = 0; index < trigger_count; ++index) {
    std::uint16_t code = 0;
    if (!tail.u16(code)) {
      return std::nullopt;
    }
    decision.revocation_triggers.push_back(static_cast<ReasonCode>(code));
  }
  if (!tail.boolean(decision.revocable) || !decode_reasons(tail, limits, decision.reasons) ||
      !tail.exhausted()) {
    return std::nullopt;
  }
  return decision;
}

std::vector<std::byte> encode_fence(const FenceRecord& fence) noexcept {
  std::vector<std::byte> bytes;
  CanonicalWriter writer(bytes);
  writer.text_id(fence.id);
  writer.u64(fence.sequence.value());
  writer.u8(static_cast<std::uint8_t>(fence.scope.kind));
  writer.text_id(fence.scope.id);
  writer.text_id(fence.partition);
  writer.u64(fence.fenced_authority_sequence.value());
  writer.u64(fence.fenced_epoch.value());
  writer.binary_id(fence.fenced_boot);
  writer.u64(fence.issuing_epoch.value());
  writer.binary_id(fence.issuing_boot);
  writer.u64(fence.tick);
  writer.u16(static_cast<std::uint16_t>(fence.cause));
  return bytes;
}

std::optional<FenceRecord> decode_fence(const std::vector<std::byte>& bytes,
                                        const Limits& limits) noexcept {
  (void)limits;
  CanonicalReader reader(bytes.data(), bytes.size());
  FenceRecord fence;
  std::uint8_t scope = 0;
  std::uint16_t cause = 0;
  if (!reader.text_id(fence.id) || !reader.counter(fence.sequence) || !reader.u8(scope) ||
      scope > scope_kind_domain_max || !reader.text_id(fence.scope.id) ||
      !reader.text_id(fence.partition) || !reader.counter(fence.fenced_authority_sequence) ||
      !reader.counter(fence.fenced_epoch) || !reader.binary_id(fence.fenced_boot) ||
      !reader.counter(fence.issuing_epoch) || !reader.binary_id(fence.issuing_boot) ||
      !reader.u64(fence.tick) || !reader.u16(cause) || !reader.exhausted()) {
    return std::nullopt;
  }
  fence.scope.kind = static_cast<ScopeKind>(scope);
  fence.cause = static_cast<ReasonCode>(cause);
  return fence;
}

// ---------------------------------------------------------------------------
// DurableStore
// ---------------------------------------------------------------------------

DurableStore::DurableStore(std::filesystem::path journal_path, std::filesystem::path snapshot_path,
                           const Limits& limits)
    : journal_path_(std::move(journal_path)),
      snapshot_path_(std::move(snapshot_path)),
      limits_(limits) {}

DurableStore::~DurableStore() { close(); }

StoreLoadResult DurableStore::load() {
  StoreLoadResult result;
  records_.clear();
  highest_sequence_ = 0;
  journal_good_bytes_ = 0;
  stats_ = StoreStats{};

  std::uint64_t snapshot_sequence = 0;
  std::error_code error;
  if (std::filesystem::exists(snapshot_path_, error)) {
    std::FILE* file = open_file(snapshot_path_, "rb");
    if (file == nullptr) {
      append_reason(result.reasons, ReasonCode::StoreIoFailure, snapshot_path_.string(),
                    "the snapshot exists but could not be opened", limits_);
      return result;
    }
    std::vector<std::byte> image;
    // The read buffer is heap allocated: a 64 KiB stack frame in a load path is
    // an unnecessary risk on small thread stacks.
    std::vector<std::byte> chunk(64 * 1024, std::byte{0});
    bool too_large = false;
    for (;;) {
      const std::size_t read = std::fread(chunk.data(), 1, chunk.size(), file);
      if (read == 0) {
        break;
      }
      if (image.size() + read > limits_.max_snapshot_bytes) {
        too_large = true;
        break;
      }
      image.insert(image.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read));
    }
    std::fclose(file);
    if (too_large) {
      append_reason(result.reasons, ReasonCode::LimitExceededReason, snapshot_path_.string(),
                    "the snapshot exceeds the configured bound", limits_);
      return result;
    }
    ReasonCode header_code = ReasonCode::Ok;
    if (!validate_header(image.data(), image.size(), snapshot_magic, header_code)) {
      append_reason(result.reasons, header_code, snapshot_path_.string(),
                    "the snapshot header is corrupt or of an unsupported version", limits_);
      return result;
    }
    if (image.size() < store_header_bytes + record_trailer_bytes) {
      append_reason(result.reasons, ReasonCode::StoreIntegrityFailure, snapshot_path_.string(),
                    "the snapshot is shorter than its header and trailer", limits_);
      return result;
    }
    if (read_u64(image.data() + image.size() - record_trailer_bytes) !=
        crc64_bytes(image.data(), image.size() - record_trailer_bytes)) {
      stats_.integrity_failures = 1;
      append_reason(result.reasons, ReasonCode::StoreIntegrityFailure, snapshot_path_.string(),
                    "the snapshot checksum does not match its contents", limits_);
      return result;
    }
    const std::byte* payload = image.data() + store_header_bytes;
    const std::size_t payload_size = image.size() - store_header_bytes - record_trailer_bytes;
    if (payload_size < 16) {
      append_reason(result.reasons, ReasonCode::StoreIntegrityFailure, snapshot_path_.string(),
                    "the snapshot payload is shorter than its own header", limits_);
      return result;
    }
    const std::uint64_t last_sequence = read_u64(payload);
    const std::uint64_t record_count = read_u64(payload + 8);
    if (record_count > limits_.max_journal_records) {
      append_reason(result.reasons, ReasonCode::LimitExceededReason, snapshot_path_.string(),
                    "the snapshot declares more records than the configured bound", limits_);
      return result;
    }
    std::size_t cursor = 16;
    for (std::uint64_t index = 0; index < record_count; ++index) {
      DurableRecord record;
      std::size_t consumed = 0;
      const auto status = parse_record(payload + cursor, payload_size - cursor,
                                       limits_.max_record_bytes, record, consumed);
      if (status != RecordParseStatus::Ok) {
        append_reason(result.reasons, ReasonCode::StoreIntegrityFailure, snapshot_path_.string(),
                      "a snapshot record is truncated, typed outside its domain, or fails its "
                      "checksum",
                      limits_);
        return result;
      }
      if (record.sequence <= highest_sequence_) {
        append_reason(result.reasons, ReasonCode::StoreSequenceRegression, snapshot_path_.string(),
                      "the snapshot contains a regressed record sequence", limits_);
        return result;
      }
      highest_sequence_ = record.sequence;
      records_.push_back(std::move(record));
      cursor += consumed;
    }
    if (cursor != payload_size) {
      stats_.trailing_garbage = true;
      append_reason(result.reasons, ReasonCode::StoreTrailingGarbage, snapshot_path_.string(),
                    "the snapshot carries bytes after its declared record stream", limits_);
      return result;
    }
    if (highest_sequence_ != last_sequence) {
      append_reason(result.reasons, ReasonCode::StoreSequenceRegression, snapshot_path_.string(),
                    "the snapshot sequence watermark does not match its records", limits_);
      return result;
    }
    snapshot_sequence = last_sequence;
    stats_.loaded_from_snapshot = true;
    result.records_loaded += record_count;
  }

  std::FILE* journal = open_file(journal_path_, "rb");
  if (journal != nullptr) {
    std::array<std::byte, store_header_bytes> header{};
    const std::size_t header_read = std::fread(header.data(), 1, header.size(), journal);
    if (header_read == 0) {
      // A journal that exists but holds nothing is a creation that was
      // interrupted before the header was committed. No record was ever
      // acknowledged from it, so it is treated as an empty journal.
      std::fclose(journal);
    } else {
      ReasonCode header_code = ReasonCode::Ok;
      if (!validate_header(header.data(), header_read, journal_magic, header_code)) {
        std::fclose(journal);
        append_reason(result.reasons, header_code, journal_path_.string(),
                      "the journal header is corrupt or of an unsupported version", limits_);
        return result;
      }
      std::uint64_t applied = highest_sequence_;
      std::uint64_t offset = store_header_bytes;
      bool torn = false;
      std::array<std::byte, record_header_bytes> record_header{};
      for (;;) {
        const std::size_t read = std::fread(record_header.data(), 1, record_header.size(), journal);
        if (read == 0) {
          break;
        }
        if (read < record_header.size()) {
          torn = true;
          break;
        }
        const auto type = static_cast<std::uint16_t>(
            (static_cast<unsigned>(record_header[0]) << 8) | static_cast<unsigned>(record_header[1]));
        if (type == 0 || type > durable_record_type_domain_max) {
          std::fclose(journal);
          stats_.integrity_failures = 1;
          append_reason(result.reasons, ReasonCode::StoreIntegrityFailure, journal_path_.string(),
                        "a journal record declares a type outside its domain", limits_);
          return result;
        }
        const std::uint32_t payload_size = read_u32(record_header.data() + 4);
        if (static_cast<std::size_t>(payload_size) > limits_.max_record_bytes) {
          std::fclose(journal);
          stats_.integrity_failures = 1;
          append_reason(result.reasons, ReasonCode::StoreIntegrityFailure, journal_path_.string(),
                        "a journal record declares a payload longer than the configured bound",
                        limits_);
          return result;
        }
        DurableRecord record;
        record.type = static_cast<DurableRecordType>(type);
        record.sequence = read_u64(record_header.data() + 8);
        record.payload.resize(payload_size);
        if (payload_size != 0 &&
            std::fread(record.payload.data(), 1, payload_size, journal) < payload_size) {
          torn = true;
          break;
        }
        std::array<std::byte, record_trailer_bytes> trailer{};
        if (std::fread(trailer.data(), 1, trailer.size(), journal) < trailer.size()) {
          torn = true;
          break;
        }
        Crc64 crc;
        crc.update(record_header.data(), record_header.size());
        crc.update(record.payload.data(), record.payload.size());
        if (read_u64(trailer.data()) != (crc.value() ^ 0xFFFF'FFFF'FFFF'FFFFull)) {
          std::fclose(journal);
          stats_.integrity_failures = 1;
          append_reason(result.reasons, ReasonCode::StoreIntegrityFailure, journal_path_.string(),
                        "a complete journal record fails its integrity check; the store is treated "
                        "as corrupt rather than silently truncated",
                        limits_);
          return result;
        }
        const std::uint64_t consumed =
            record_header_bytes + static_cast<std::uint64_t>(payload_size) + record_trailer_bytes;
        if (record.sequence <= snapshot_sequence) {
          // Already contained in the snapshot. This is the expected state after
          // a snapshot replacement that was interrupted before the journal was
          // truncated, so it is recovered rather than rejected.
          offset += consumed;
          continue;
        }
        if (record.sequence <= applied) {
          std::fclose(journal);
          append_reason(result.reasons, ReasonCode::StoreSequenceRegression, journal_path_.string(),
                        "a journal record regresses the durable sequence", limits_);
          return result;
        }
        applied = record.sequence;
        highest_sequence_ = record.sequence;
        ++result.records_loaded;
        ++stats_.replayed_records;
        records_.push_back(std::move(record));
        offset += consumed;
      }
      if (torn) {
        std::fseek(journal, 0, SEEK_END);
        const long end = std::ftell(journal);
        const auto end_value = static_cast<std::uint64_t>(end < 0 ? 0 : end);
        stats_.torn_tail_bytes = end_value > offset ? end_value - offset : 0;
        result.torn_tail_recovered = true;
        append_reason(result.reasons, ReasonCode::StoreTornTailRecovered, journal_path_.string(),
                      "the final journal record was never fully committed and was discarded",
                      limits_);
      }
      journal_good_bytes_ = offset;
      std::fclose(journal);
    }
  }

  stats_.highest_sequence = highest_sequence_;
  result.ok = true;
  append_reason(result.reasons, ReasonCode::DurableCommitAdvanced, journal_path_.string(),
                "durable state loaded and validated", limits_);
  return result;
}

bool DurableStore::open_for_append() {
  if (journal_ != nullptr) {
    return true;
  }
  std::error_code error;
  const auto parent = journal_path_.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
  }
  std::FILE* file = open_file(journal_path_, "r+b");
  if (file == nullptr) {
    file = open_file(journal_path_, "w+b");
    if (file == nullptr) {
      return false;
    }
    const std::vector<std::byte> header = make_header(journal_magic);
    if (std::fwrite(header.data(), 1, header.size(), file) != header.size()) {
      std::fclose(file);
      return false;
    }
    stats_.bytes_written += header.size();
    journal_good_bytes_ = header.size();
  } else if (journal_good_bytes_ > 0) {
    // Discard a torn tail so that the next append cannot follow garbage.
    const auto size = std::filesystem::file_size(journal_path_, error);
    if (!error && size > journal_good_bytes_) {
      if (!truncate_file(file, journal_good_bytes_)) {
        std::fclose(file);
        return false;
      }
    }
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return false;
  }
  journal_ = file;
  stats_.open = true;
  return true;
}

bool DurableStore::append(DurableRecordType type, std::uint64_t sequence,
                          const std::vector<std::byte>& payload) {
  if (journal_ == nullptr) {
    return false;
  }
  if (payload.size() > limits_.max_record_bytes) {
    return false;
  }
  if (sequence <= highest_sequence_) {
    return false;
  }
  DurableRecord record;
  record.type = type;
  record.sequence = sequence;
  record.payload = payload;
  const std::vector<std::byte> bytes = encode_record(record);
  auto* file = static_cast<std::FILE*>(journal_);
  // The record is written with a single fwrite so that a torn write is always a
  // truncated final record, never a spliced one.
  if (std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
    return false;
  }
  if (std::fflush(file) != 0 || !sync_file(file)) {
    return false;
  }
  highest_sequence_ = sequence;
  journal_good_bytes_ += bytes.size();
  stats_.bytes_written += bytes.size();
  stats_.appended_records += 1;
  stats_.highest_sequence = highest_sequence_;
  records_.push_back(std::move(record));
  return true;
}

bool DurableStore::write_snapshot() {
  std::vector<std::byte> payload;
  CanonicalWriter writer(payload);
  writer.u64(highest_sequence_);
  writer.u64(static_cast<std::uint64_t>(records_.size()));
  for (const DurableRecord& record : records_) {
    const std::vector<std::byte> bytes = encode_record(record);
    writer.bytes(bytes.data(), bytes.size());
  }

  std::vector<std::byte> image = make_header(snapshot_magic);
  image.insert(image.end(), payload.begin(), payload.end());
  std::array<std::byte, record_trailer_bytes> trailer{};
  write_u64(trailer.data(), crc64_bytes(image.data(), image.size()));
  image.insert(image.end(), trailer.begin(), trailer.end());

  if (image.size() > limits_.max_snapshot_bytes) {
    return false;
  }

  const std::filesystem::path temporary = snapshot_path_.string() + ".tmp";
  std::FILE* file = open_file(temporary, "wb");
  if (file == nullptr) {
    return false;
  }
  const bool written = std::fwrite(image.data(), 1, image.size(), file) == image.size();
  const bool flushed = written && std::fflush(file) == 0 && sync_file(file);
  std::fclose(file);
  if (!flushed) {
    std::error_code error;
    std::filesystem::remove(temporary, error);
    return false;
  }
  std::error_code error;
  std::filesystem::rename(temporary, snapshot_path_, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  stats_.snapshot_writes += 1;
  stats_.bytes_written += image.size();
  return true;
}

bool DurableStore::truncate_journal() {
  if (journal_ == nullptr) {
    return false;
  }
  auto* file = static_cast<std::FILE*>(journal_);
  if (std::fflush(file) != 0) {
    return false;
  }
  std::fclose(file);
  journal_ = nullptr;
  const std::vector<std::byte> header = make_header(journal_magic);
  std::FILE* fresh = open_file(journal_path_, "wb");
  if (fresh == nullptr) {
    return false;
  }
  const bool ok = std::fwrite(header.data(), 1, header.size(), fresh) == header.size() &&
                  std::fflush(fresh) == 0 && sync_file(fresh);
  std::fclose(fresh);
  if (!ok) {
    return false;
  }
  journal_good_bytes_ = header.size();
  stats_.bytes_written += header.size();
  return open_for_append();
}

bool DurableStore::compact() {
  if (!write_snapshot()) {
    return false;
  }
  return truncate_journal();
}

void DurableStore::close() {
  if (journal_ != nullptr) {
    auto* file = static_cast<std::FILE*>(journal_);
    std::fflush(file);
    sync_file(file);
    std::fclose(file);
    journal_ = nullptr;
  }
  stats_.open = false;
}

}  // namespace fabric_partition_manager
