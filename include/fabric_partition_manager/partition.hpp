// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Canonical partitions.
//
// A partition is a maximal set of components that current evidence proves
// mutually reachable. Its identity is derived from canonical inputs only:
//
//   MembershipDigest = H(domain, count, sorted member identifiers)
//   PartitionId      = H(domain, lineage, generation, MembershipDigest)
//
// Two runtimes that observe the same membership in different orders, or that
// discover the members in a different sequence, derive byte-identical
// identities. Partition identity is generation bound: the same members under a
// later generation are a different partition identity with the same lineage.
#ifndef FABRIC_PARTITION_MANAGER_PARTITION_HPP
#define FABRIC_PARTITION_MANAGER_PARTITION_HPP

#include <cstdint>
#include <string_view>
#include <vector>

#include "fabric_partition_manager/authority.hpp"
#include "fabric_partition_manager/evidence.hpp"
#include "fabric_partition_manager/lineage.hpp"
#include "fabric_partition_manager/limits.hpp"
#include "fabric_partition_manager/strong_id.hpp"

namespace fabric_partition_manager {

enum class PartitionCertainty : std::uint8_t {
  // The component decomposition is the best supported by current evidence, but
  // at least one component-crossing pair is not proven unreachable. Unknown
  // connectivity may still merge components.
  Partial = 0,
  // Every component-crossing ordered pair is proven unreachable. No unknown
  // connectivity can change the decomposition.
  Confirmed = 1,
};
inline constexpr std::uint8_t partition_certainty_domain_max = 1;

enum class PartitionLifecycle : std::uint8_t {
  // Present in the latest assessment; not yet committed to durable lineage.
  Discovered = 0,
  // Recorded in durable lineage.
  Committed = 1,
  // Loaded from durability. Requires revalidation before it can hold authority
  // again; its pre-restart authority is not restored.
  Interrupted = 2,
  // Replaced by a later generation of the same lineage.
  Superseded = 3,
  // Absorbed into a governed merge.
  Merged = 4,
  // No longer present in the fabric.
  Retired = 5,
};
inline constexpr std::uint8_t partition_lifecycle_domain_max = 5;

[[nodiscard]] std::string_view partition_certainty_name(PartitionCertainty value) noexcept;
[[nodiscard]] std::string_view partition_lifecycle_name(PartitionLifecycle value) noexcept;

struct Partition {
  PartitionId id;
  MembershipDigest membership;
  PartitionGeneration generation;
  LineageId lineage;
  std::vector<ComponentId> members;
  std::vector<std::uint64_t> weights;
  std::uint64_t total_weight = 0;
  std::uint32_t voter_count = 0;
  PartitionCertainty certainty = PartitionCertainty::Partial;
  PartitionLifecycle lifecycle = PartitionLifecycle::Discovered;
  AuthorityVector authority;
  // The authority sequence issued to this partition by the current assessment.
  // Zero when the partition holds no authority. It is the exact value a fence
  // names when the grant is revoked.
  AuthoritySequence authority_sequence;
  // The single live lineage this membership descends from, when it descends
  // from exactly one. Nil for a genesis membership and for a pending merge.
  LineageId parent_lineage;
  // True when this partition is the single holder of fabric authority for the
  // current assessment.
  bool authoritative = false;
  // Non-empty when this component set spans two or more live lineages and no
  // governed merge has been committed for it. Such a partition may not be
  // granted authority above observe-only.
  std::vector<LineageId> pending_merge_parents;
  std::vector<Reason> reasons;

  [[nodiscard]] std::uint64_t member_count() const noexcept {
    return static_cast<std::uint64_t>(members.size());
  }
};

[[nodiscard]] PartitionId compute_partition_id(const LineageId& lineage,
                                               PartitionGeneration generation,
                                               const MembershipDigest& membership) noexcept;

// Deterministic total order over partitions: lexicographic by the canonical
// member list. Membership sets are distinct by construction, so this is a total
// order with no ties.
[[nodiscard]] bool partition_canonical_less(const Partition& lhs, const Partition& rhs) noexcept;

// Canonical ordering of a member list. Exposed so callers can canonicalise a
// membership before deriving an identity from it.
void canonicalise_members(std::vector<ComponentId>& members);

struct PartitionBuildInput {
  const ReachabilitySnapshot* snapshot = nullptr;
  PartitionGeneration generation;
  const PartitionPolicy* policy = nullptr;
  const LineageStore* lineage = nullptr;
  std::uint64_t tick = 0;
};

// Builds one Partition per component of the snapshot, in canonical order. The
// authority vector is populated at the OBSERVATION stage only; authority is a
// separate, explicit step.
[[nodiscard]] std::vector<Partition> build_partitions(const PartitionBuildInput& input,
                                                      const Limits& limits);

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_PARTITION_HPP
