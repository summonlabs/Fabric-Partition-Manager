// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// The authoritative component roster.
//
// Fabric Partition Manager does not discover topology. It consumes the
// authoritative roster published by Fabric Topology / Fabric Registry and owns
// only the partition-specific interpretation of it. The roster is stored in
// canonical order, so every downstream identity, digest and explanation is
// independent of the order in which the definition happened to arrive.
#ifndef FABRIC_PARTITION_MANAGER_TOPOLOGY_HPP
#define FABRIC_PARTITION_MANAGER_TOPOLOGY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "fabric_partition_manager/digest.hpp"
#include "fabric_partition_manager/error.hpp"
#include "fabric_partition_manager/limits.hpp"
#include "fabric_partition_manager/strong_id.hpp"

namespace fabric_partition_manager {

struct TopologyDefinition {
  TopologyId id;
  TopologyGeneration generation;
  Provenance provenance;
  std::vector<ComponentId> components;
};

class ComponentRoster {
 public:
  ComponentRoster() = default;

  // Sorts the roster into canonical order. A roster that repeats a component is
  // rejected: an authoritative definition that names the same component twice
  // is malformed, not a roster that can be silently de-duplicated.
  [[nodiscard]] static std::optional<ComponentRoster> create(const TopologyDefinition& definition,
                                                             const Limits& limits);

  [[nodiscard]] bool empty() const noexcept { return components_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return components_.size(); }
  [[nodiscard]] const std::vector<ComponentId>& components() const noexcept { return components_; }
  [[nodiscard]] const TopologyId& id() const noexcept { return id_; }
  [[nodiscard]] TopologyGeneration generation() const noexcept { return generation_; }
  [[nodiscard]] const Provenance& provenance() const noexcept { return provenance_; }

  [[nodiscard]] bool contains(const ComponentId& component) const;
  [[nodiscard]] std::optional<std::uint32_t> index_of(const ComponentId& component) const;

  // True when the roster already carries a definition. An empty default value
  // is unset; a created roster with zero components is set but empty.
  [[nodiscard]] bool is_set() const noexcept { return set_; }

  // Canonical digest over (topology id, generation, provenance, sorted roster).
  [[nodiscard]] const MembershipDigest& digest() const noexcept { return digest_; }

  [[nodiscard]] std::uint64_t total_weight(const std::vector<std::uint64_t>& weights) const noexcept;

 private:
  TopologyId id_;
  TopologyGeneration generation_;
  Provenance provenance_;
  std::vector<ComponentId> components_;
  std::unordered_map<ComponentId, std::uint32_t> index_;
  MembershipDigest digest_{};
  bool set_ = false;
};

[[nodiscard]] MembershipDigest compute_roster_digest(const TopologyDefinition& definition) noexcept;
[[nodiscard]] MembershipDigest compute_membership_digest(const std::vector<ComponentId>& sorted_members) noexcept;

}  // namespace fabric_partition_manager

#endif  // FABRIC_PARTITION_MANAGER_TOPOLOGY_HPP
