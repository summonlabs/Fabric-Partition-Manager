// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Bounded tables, checked arithmetic and exact accounting.
//
// Every externally reachable allocation in Fabric Partition Manager is derived
// from a value that has already been compared against a configured limit, and
// every size computation is performed with checked addition and multiplication.
// A hostile or corrupt input therefore produces a deterministic refusal, never
// a wraparound, an over-allocation or process instability.
#ifndef FABRIC_PARTITION_MANAGER_LIMITS_HPP
#define FABRIC_PARTITION_MANAGER_LIMITS_HPP

#include <cstddef>
#include <cstdint>
#include <optional>

namespace fabric_partition_manager {

// The exact bounds a runtime instance enforces. Limits are captured at
// construction so a caller can tighten them; a store written under wider limits
// remains loadable, because limits bound what a process will *add*, not what it
// will *load* and validate.
struct Limits {
  // Fabric scope and evidence.
  std::size_t max_components = 1'000'000;
  std::size_t max_observations_per_evidence = 8'000'000;
  std::size_t max_evidence_bytes = 256u * 1024u * 1024u;
  std::size_t max_retained_evidence = 4096;
  std::size_t max_covered_components_per_evidence = 1'000'000;

  // Partition computation.
  std::size_t max_partitions = 1'000'000;
  std::size_t max_reasons_per_decision = 64;
  std::size_t max_members_in_reason_subject = 8;

  // Retained durable and in-memory history.
  std::size_t max_decision_history = 4096;
  std::size_t max_fence_records = 4096;
  std::size_t max_lineage_records = 16'384;
  // Exact bound on the total number of component entries retained across every
  // lineage record. Lineage membership is stored exactly rather than sampled,
  // so ancestry is never indeterminate merely because a partition is large.
  std::size_t max_lineage_member_entries = 2'000'000;
  std::size_t max_authority_grants_retained = 4096;
  std::size_t max_attempt_records = 4096;
  std::size_t max_revalidations_retained = 4096;

  // Bounded search. Ancestry reconciliation walks the lineage graph upward.
  // When this node budget is exhausted the outcome is INDETERMINATE
  // (SEARCH_LIMIT_REACHED), never "unrelated".
  std::size_t max_lineage_walk_nodes = 8192;
  std::size_t max_lineage_walk_depth = 256;

  // Persistence.
  std::size_t max_snapshot_bytes = 128u * 1024u * 1024u;
  std::size_t max_record_bytes = 64u * 1024u * 1024u;
  std::size_t max_journal_records = 200'000;

  // Transport.
  std::size_t max_frame_bytes = 1u * 1024u * 1024u;
  std::size_t max_sessions = 64;
  std::size_t max_pending_connections = 64;
  std::size_t max_inflight_requests_per_session = 64;
  std::size_t max_response_queue = 256;

  // Text.
  std::size_t max_text_bytes = 192;
  std::size_t max_provenance_bytes = 96;
};

[[nodiscard]] const Limits& default_limits() noexcept;

// ---------------------------------------------------------------------------
// Checked arithmetic. Every function returns nullopt instead of wrapping.
// ---------------------------------------------------------------------------

[[nodiscard]] std::optional<std::uint64_t> checked_add(std::uint64_t lhs, std::uint64_t rhs) noexcept;
[[nodiscard]] std::optional<std::uint64_t> checked_mul(std::uint64_t lhs, std::uint64_t rhs) noexcept;
[[nodiscard]] std::optional<std::uint32_t> checked_narrow_u32(std::uint64_t value) noexcept;
[[nodiscard]] std::optional<std::size_t> checked_size_add(std::size_t lhs, std::size_t rhs) noexcept;
[[nodiscard]] std::optional<std::size_t> checked_size_mul(std::size_t lhs, std::size_t rhs) noexcept;

// Number of unordered pairs in a population of count distinct elements,
// i.e. count * (count - 1) / 2, computed without intermediate overflow.
[[nodiscard]] std::optional<std::uint64_t> unordered_pair_count(std::uint64_t count) noexcept;

// Number of ordered pairs, i.e. count * (count - 1).
[[nodiscard]] std::optional<std::uint64_t> ordered_pair_count(std::uint64_t count) noexcept;

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_LIMITS_HPP
