// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/topology.hpp"

#include <algorithm>

#include "fabric_partition_manager/encoding.hpp"

namespace fabric_partition_manager {

MembershipDigest compute_roster_digest(const TopologyDefinition& definition) noexcept {
  Sha256 hasher;
  hasher.update_text("fabric-partition-manager/roster/v1");
  hasher.update_text(definition.id.view());
  hasher.update_be64(definition.generation.value());
  hasher.update_text(definition.provenance.view());

  // The roster is digested in canonical order. Members are sorted here so that
  // a caller which supplied an unsorted definition still obtains the canonical
  // digest rather than an order-dependent one.
  std::vector<ComponentId> sorted = definition.components;
  std::sort(sorted.begin(), sorted.end());
  hasher.update_be64(static_cast<std::uint64_t>(sorted.size()));
  for (const ComponentId& component : sorted) {
    hasher.update_text(component.view());
  }
  return MembershipDigest::from_digest(hasher.finish());
}

MembershipDigest compute_membership_digest(const std::vector<ComponentId>& sorted_members) noexcept {
  Sha256 hasher;
  hasher.update_text("fabric-partition-manager/membership/v1");
  hasher.update_be64(static_cast<std::uint64_t>(sorted_members.size()));
  for (const ComponentId& component : sorted_members) {
    hasher.update_text(component.view());
  }
  return MembershipDigest::from_digest(hasher.finish());
}

std::optional<ComponentRoster> ComponentRoster::create(const TopologyDefinition& definition,
                                                       const Limits& limits) {
  if (definition.id.is_nil()) {
    return std::nullopt;
  }
  if (definition.components.size() > limits.max_components) {
    return std::nullopt;
  }

  ComponentRoster roster;
  roster.id_ = definition.id;
  roster.generation_ = definition.generation;
  roster.provenance_ = definition.provenance;
  roster.components_ = definition.components;
  std::sort(roster.components_.begin(), roster.components_.end());

  std::size_t unique_count = 0;
  for (std::size_t index = 0; index < roster.components_.size(); ++index) {
    if (roster.components_[index].is_nil()) {
      return std::nullopt;
    }
    if (index > 0 && roster.components_[index] == roster.components_[index - 1]) {
      // An authoritative roster that repeats a component is malformed. Silently
      // de-duplicating would hide a defect in the producer.
      return std::nullopt;
    }
    ++unique_count;
  }

  roster.index_.reserve(roster.components_.size());
  for (std::size_t index = 0; index < roster.components_.size(); ++index) {
    roster.index_.emplace(roster.components_[index], static_cast<std::uint32_t>(index));
  }
  roster.digest_ = compute_roster_digest(definition);
  roster.set_ = true;
  (void)unique_count;
  return roster;
}

bool ComponentRoster::contains(const ComponentId& component) const {
  return index_.find(component) != index_.end();
}

std::optional<std::uint32_t> ComponentRoster::index_of(const ComponentId& component) const {
  const auto found = index_.find(component);
  if (found == index_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::uint64_t ComponentRoster::total_weight(const std::vector<std::uint64_t>& weights) const noexcept {
  std::uint64_t total = 0;
  const std::size_t count = weights.size() < components_.size() ? weights.size() : components_.size();
  for (std::size_t index = 0; index < count; ++index) {
    total += weights[index];
  }
  return total;
}

}  // namespace fabric_partition_manager
