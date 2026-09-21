// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/partition.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

#include "fabric_partition_manager/encoding.hpp"

namespace fabric_partition_manager {

std::string_view partition_certainty_name(PartitionCertainty value) noexcept {
  switch (value) {
    case PartitionCertainty::Partial: return "PARTIAL";
    case PartitionCertainty::Confirmed: return "CONFIRMED";
  }
  return "UNRECOGNIZED";
}

std::string_view partition_lifecycle_name(PartitionLifecycle value) noexcept {
  switch (value) {
    case PartitionLifecycle::Discovered: return "DISCOVERED";
    case PartitionLifecycle::Committed: return "COMMITTED";
    case PartitionLifecycle::Interrupted: return "INTERRUPTED";
    case PartitionLifecycle::Superseded: return "SUPERSEDED";
    case PartitionLifecycle::Merged: return "MERGED";
    case PartitionLifecycle::Retired: return "RETIRED";
  }
  return "UNRECOGNIZED";
}

PartitionId compute_partition_id(const LineageId& lineage, PartitionGeneration generation,
                                 const MembershipDigest& membership) noexcept {
  Sha256 hasher;
  hasher.update_text("fabric-partition-manager/partition-identity/v1");
  hasher.update_text(lineage.view());
  hasher.update_be64(generation.value());
  hasher.update(membership.value().bytes.data(), digest_bytes);
  const std::string text = std::string("p") + hasher.finish().hex().substr(0, 32);
  return PartitionId::from_validated(text);
}

bool partition_canonical_less(const Partition& lhs, const Partition& rhs) noexcept {
  const std::size_t shared = std::min(lhs.members.size(), rhs.members.size());
  for (std::size_t index = 0; index < shared; ++index) {
    if (lhs.members[index] < rhs.members[index]) {
      return true;
    }
    if (rhs.members[index] < lhs.members[index]) {
      return false;
    }
  }
  return lhs.members.size() < rhs.members.size();
}

void canonicalise_members(std::vector<ComponentId>& members) {
  std::sort(members.begin(), members.end());
  members.erase(std::unique(members.begin(), members.end()), members.end());
}

namespace {

enum class LineageClassification : std::uint8_t {
  Genesis = 0,
  Inherit = 1,
  Split = 2,
  Divergent = 3,
  PendingMerge = 4,
};

}  // namespace

std::vector<Partition> build_partitions(const PartitionBuildInput& input, const Limits& limits) {
  std::vector<Partition> result;
  if (input.snapshot == nullptr || !input.snapshot->valid) {
    return result;
  }
  const ReachabilitySnapshot& snapshot = *input.snapshot;
  const std::size_t component_count = snapshot.components.size();
  if (component_count == 0) {
    return result;
  }
  if (snapshot.component_representatives.size() > limits.max_partitions) {
    // Refusing to build is the correct response to exceeding the partition
    // bound. Truncating the decomposition would silently drop live components.
    return result;
  }

  std::vector<std::uint32_t> ordinal(component_count, 0);
  for (std::size_t index = 0; index < snapshot.component_representatives.size(); ++index) {
    ordinal[snapshot.component_representatives[index]] = static_cast<std::uint32_t>(index);
  }
  std::vector<std::vector<std::uint32_t>> groups(snapshot.component_representatives.size());
  for (std::size_t index = 0; index < component_count; ++index) {
    groups[ordinal[snapshot.component_of[index]]].push_back(static_cast<std::uint32_t>(index));
  }

  // Live lineage view: member -> owning live lineages.
  std::unordered_map<ComponentId, std::vector<LineageId>> member_owners;
  std::vector<LineageId> live;
  if (input.lineage != nullptr) {
    live = input.lineage->live_lineages();
    for (const LineageId& lineage : live) {
      const LineageRecord* record = input.lineage->latest(lineage);
      if (record == nullptr) {
        continue;
      }
      for (const ComponentId& member : record->members) {
        member_owners[member].push_back(lineage);
      }
    }
  }

  result.reserve(groups.size());
  for (const std::vector<std::uint32_t>& group : groups) {
    Partition partition;
    partition.members.reserve(group.size());
    partition.weights.reserve(group.size());
    for (const std::uint32_t index : group) {
      partition.members.push_back(snapshot.components[index]);
      const std::uint64_t weight = snapshot.weights[index];
      partition.weights.push_back(weight);
      partition.total_weight += weight;
      if (snapshot.voters[index]) {
        ++partition.voter_count;
      }
    }
    partition.membership = compute_membership_digest(partition.members);
    partition.certainty =
        snapshot.confirmed ? PartitionCertainty::Confirmed : PartitionCertainty::Partial;
    partition.lifecycle = PartitionLifecycle::Discovered;

    std::vector<LineageId> intersecting;
    if (!member_owners.empty()) {
      for (const ComponentId& member : partition.members) {
        const auto found = member_owners.find(member);
        if (found == member_owners.end()) {
          continue;
        }
        for (const LineageId& owner : found->second) {
          intersecting.push_back(owner);
        }
      }
      std::sort(intersecting.begin(), intersecting.end());
      intersecting.erase(std::unique(intersecting.begin(), intersecting.end()), intersecting.end());
    }

    LineageClassification classification = LineageClassification::Genesis;
    LineageId lineage;
    if (intersecting.empty()) {
      lineage = genesis_lineage_id(partition.membership);
    } else if (intersecting.size() == 1 && input.lineage != nullptr) {
      const LineageRecord* record = input.lineage->latest(intersecting.front());
      if (record != nullptr && record->membership == partition.membership) {
        classification = LineageClassification::Inherit;
        lineage = intersecting.front();
      } else if (record != nullptr &&
                 std::includes(record->members.begin(), record->members.end(),
                               partition.members.begin(), partition.members.end())) {
        classification = LineageClassification::Split;
        lineage = split_lineage_id(intersecting.front(), partition.membership);
        partition.parent_lineage = intersecting.front();
      } else {
        classification = LineageClassification::Divergent;
        lineage = derive_lineage_id("divergent", intersecting.front(), LineageId{},
                                    partition.membership);
        partition.parent_lineage = intersecting.front();
      }
    } else {
      // The component set spans two or more live lineages. That is a merge
      // candidate, and a merge is a governed reconciliation event: this
      // partition receives a provisional identity and may not be granted
      // authority above observe-only until the merge is decided.
      classification = LineageClassification::PendingMerge;
      LineageId folded = intersecting.front();
      for (std::size_t index = 1; index < intersecting.size(); ++index) {
        folded = merge_lineage_id(folded, intersecting[index]);
      }
      lineage = derive_lineage_id("pending-merge", folded, LineageId{}, partition.membership);
      partition.pending_merge_parents = intersecting;
    }

    partition.lineage = lineage;
    partition.generation = input.generation;
    partition.id = compute_partition_id(lineage, input.generation, partition.membership);
    partition.authority = observed_only_authority(CapabilitySet::none(), partition.reasons);

    append_reason(partition.reasons, ReasonCode::PartitionDetected, partition.id.view(),
                  "component set detected in the current reachability decomposition", limits);
    append_reason(partition.reasons, ReasonCode::PartitionMembershipCanonical,
                  partition.id.view(),
                  "membership digest derived from the canonically ordered member set", limits);
    append_reason(partition.reasons,
                  partition.certainty == PartitionCertainty::Confirmed
                      ? ReasonCode::PartitionConfirmed
                      : ReasonCode::PartitionPartial,
                  partition.id.view(),
                  std::to_string(partition.member_count()) + " members", limits);
    switch (classification) {
      case LineageClassification::Genesis:
        append_reason(partition.reasons, ReasonCode::MergeNewLineageEstablished,
                      partition.id.view(), "no prior lineage intersects this membership", limits);
        break;
      case LineageClassification::Inherit:
        append_reason(partition.reasons, ReasonCode::MergeInheritedLineage, partition.id.view(),
                      "membership is unchanged from the live lineage", limits);
        break;
      case LineageClassification::Split:
        append_reason(partition.reasons, ReasonCode::PartitionGenerationAdvanced,
                      partition.id.view(), "membership is a strict subset of a live lineage",
                      limits);
        break;
      case LineageClassification::Divergent:
        append_reason(partition.reasons, ReasonCode::MergeRejectedLineageConflict,
                      partition.id.view(),
                      "membership only partially overlaps a live lineage; lineage diverged", limits);
        break;
      case LineageClassification::PendingMerge:
        append_reason(partition.reasons, ReasonCode::MergeRejectedLineageConflict,
                      partition.id.view(),
                      "membership spans multiple live lineages and requires a governed merge",
                      limits);
        break;
    }
    result.push_back(std::move(partition));
  }

  std::sort(result.begin(), result.end(), partition_canonical_less);
  return result;
}

}  // namespace fabric_partition_manager
