// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Durable partition lineage.
//
// Lineage is the only part of a partition that legitimately survives a restart.
// It records which membership became which membership, through which governed
// event, under which coordinator epoch. It never records liveness, reachability
// freshness or live authority: those are re-established by evidence after every
// restart, or explicitly reported as interrupted.
//
// Ancestry queries are bounded. When the node budget is exhausted the result is
// INDETERMINATE, never "unrelated".
#ifndef FABRIC_PARTITION_MANAGER_LINEAGE_HPP
#define FABRIC_PARTITION_MANAGER_LINEAGE_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "fabric_partition_manager/digest.hpp"
#include "fabric_partition_manager/limits.hpp"
#include "fabric_partition_manager/strong_id.hpp"
#include "fabric_partition_manager/topology.hpp"

namespace fabric_partition_manager {

enum class LineageEventKind : std::uint8_t {
  Genesis = 0,
  Split = 1,
  Merge = 2,
  Revalidate = 3,
  Retire = 4,
};
inline constexpr std::uint8_t lineage_event_kind_domain_max = 4;

[[nodiscard]] std::string_view lineage_event_kind_name(LineageEventKind value) noexcept;

enum class LineageRelation : std::uint8_t {
  Identical = 0,
  AncestorOf = 1,
  DescendantOf = 2,
  // Both lineages share a common ancestor but neither contains the other. Their
  // authority histories cannot be unioned by simple set union.
  Divergent = 3,
  // No common ancestor was found within the explored ancestry. This is a
  // positive result: both ancestries were fully enumerated under the bound.
  Unrelated = 4,
  // The bounded ancestry walk hit its node or depth budget before resolving the
  // relation. This is NOT a negative result.
  Indeterminate = 5,
};
inline constexpr std::uint8_t lineage_relation_domain_max = 5;

[[nodiscard]] std::string_view lineage_relation_name(LineageRelation value) noexcept;

struct LineageRecord {
  LineageSequence sequence;
  LineageId lineage;
  PartitionGeneration generation;
  MembershipDigest membership;
  std::vector<ComponentId> members;
  LineageEventKind event = LineageEventKind::Genesis;
  LineageId parent_left;
  LineageId parent_right;
  CoordinatorEpoch epoch;
  CoordinatorBootId boot;
  DecisionId decision;
  std::uint64_t tick = 0;
  LineageDigest digest;

  [[nodiscard]] std::uint64_t member_count() const noexcept {
    return static_cast<std::uint64_t>(members.size());
  }
};

[[nodiscard]] LineageDigest compute_lineage_digest(const LineageRecord& record) noexcept;

// Canonical lineage identities. All of them are derived from canonical inputs
// only, so two runtimes that see the same memberships in a different order
// derive the same lineage identity.
[[nodiscard]] LineageId derive_lineage_id(std::string_view domain, const LineageId& left,
                                          const LineageId& right,
                                          const MembershipDigest& membership) noexcept;
[[nodiscard]] LineageId genesis_lineage_id(const MembershipDigest& membership) noexcept;
[[nodiscard]] LineageId split_lineage_id(const LineageId& parent,
                                         const MembershipDigest& membership) noexcept;
[[nodiscard]] LineageId merge_lineage_id(const LineageId& left, const LineageId& right) noexcept;

class LineageStore {
 public:
  explicit LineageStore(const Limits& limits);

  // Appends a record. Rejects sequence regression, duplicate (lineage,
  // generation) pairs, a membership digest that does not match the member list,
  // a missing parent, unsorted or repeated members, and every bound overflow.
  [[nodiscard]] bool append(const LineageRecord& record);

  [[nodiscard]] const std::vector<LineageRecord>& records() const noexcept { return records_; }
  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
  [[nodiscard]] LineageSequence highest_sequence() const noexcept { return highest_sequence_; }
  [[nodiscard]] std::uint64_t retained_member_entries() const noexcept {
    return retained_member_entries_;
  }
  [[nodiscard]] bool contains(const LineageId& lineage) const;
  [[nodiscard]] const LineageRecord* latest(const LineageId& lineage) const;
  [[nodiscard]] const LineageRecord* earliest(const LineageId& lineage) const;
  [[nodiscard]] std::vector<LineageId> live_lineages() const;

  // Bounded ancestry relation. The walk is deterministic: the frontier is
  // expanded in canonical lineage order, so the same graph and the same bounds
  // always produce the same answer.
  [[nodiscard]] LineageRelation relate(const LineageId& left, const LineageId& right,
                                       bool& limit_reached) const;

  void clear() noexcept;

 private:
  [[nodiscard]] bool has_common_ancestor(const LineageId& left, const LineageId& right) const;

  struct LineageSlot {
    std::size_t earliest = 0;
    std::size_t latest = 0;
  };

  Limits limits_;
  std::vector<LineageRecord> records_;
  // Lookup indexes. The store is append-only and records carry strictly
  // increasing sequences, so the first and the last inserted index of a lineage
  // are its earliest and its latest record. Without these indexes every lookup
  // would scan the whole store, which is quadratic in the number of lineages
  // and is exactly the accidental O(N^2) the scale suite exists to catch.
  std::map<LineageId, LineageSlot> slots_;
  std::set<std::pair<LineageId, std::uint64_t>> generations_;
  LineageSequence highest_sequence_;
  std::uint64_t retained_member_entries_ = 0;
};

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_LINEAGE_HPP
