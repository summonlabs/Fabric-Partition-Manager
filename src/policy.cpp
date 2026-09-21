// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include "fabric_partition_manager/policy.hpp"

#include <algorithm>
#include <array>

#include "fabric_partition_manager/version.hpp"

namespace fabric_partition_manager {
namespace {

constexpr std::array<std::string_view, authority_capability_count> kCapabilityNames = {
    "observe", "serve-reads", "serve-writes", "admit-mutations",
    "commit-durability", "issue-leases", "publish-decisions"};

}  // namespace

std::string_view authority_capability_name(AuthorityCapability capability) noexcept {
  const auto index = static_cast<std::size_t>(capability);
  if (index >= kCapabilityNames.size()) {
    return "unknown";
  }
  return kCapabilityNames[index];
}

std::string CapabilitySet::render() const {
  std::string result;
  for (std::size_t index = 0; index < authority_capability_count; ++index) {
    if ((bits_ & (1u << index)) == 0u) {
      continue;
    }
    if (!result.empty()) {
      result.push_back('|');
    }
    result.append(kCapabilityNames[index]);
  }
  if (result.empty()) {
    return "none";
  }
  return result;
}

bool QuorumRequirement::satisfied_by(std::uint32_t components, std::uint32_t voters,
                                     std::uint64_t weight) const noexcept {
  if (components < min_components || voters < min_voters || weight < min_weight) {
    return false;
  }
  if (require_strict_majority) {
    // Overflow-free form of (weight * 2 > domain_weight). A retained weight
    // larger than the whole domain cannot occur, but if it did it would still
    // be a majority rather than a wrapped comparison.
    if (weight > domain_weight) {
      return true;
    }
    return weight > domain_weight - weight;
  }
  return true;
}

std::uint64_t PartitionPolicy::weight_of(const ComponentId& component) const noexcept {
  const auto found = std::lower_bound(
      weights.begin(), weights.end(), component,
      [](const ComponentWeight& entry, const ComponentId& key) { return entry.component < key; });
  if (found == weights.end() || !(found->component == component)) {
    return 0;
  }
  return found->weight;
}

bool PartitionPolicy::is_voter(const ComponentId& component) const noexcept {
  const auto found = std::lower_bound(
      weights.begin(), weights.end(), component,
      [](const ComponentWeight& entry, const ComponentId& key) { return entry.component < key; });
  if (found == weights.end() || !(found->component == component)) {
    return false;
  }
  return found->voter;
}

std::uint64_t PartitionPolicy::domain_weight() const noexcept {
  std::uint64_t total = 0;
  for (const ComponentWeight& entry : weights) {
    total += entry.weight;
  }
  return total;
}

std::uint32_t PartitionPolicy::voter_capacity() const noexcept {
  std::uint32_t total = 0;
  for (const ComponentWeight& entry : weights) {
    if (entry.voter) {
      ++total;
    }
  }
  return total;
}

std::optional<PartitionPolicy> PartitionPolicy::canonicalised(const Limits& limits) const {
  if (id.is_nil()) {
    return std::nullopt;
  }
  if (schema_version != policy_schema_version) {
    return std::nullopt;
  }
  if (weights.size() > limits.max_components) {
    return std::nullopt;
  }
  PartitionPolicy result = *this;
  std::sort(result.weights.begin(), result.weights.end(),
            [](const ComponentWeight& lhs, const ComponentWeight& rhs) {
              return lhs.component < rhs.component;
            });
  for (std::size_t index = 0; index < result.weights.size(); ++index) {
    if (result.weights[index].component.is_nil()) {
      return std::nullopt;
    }
    if (index > 0 && result.weights[index].component == result.weights[index - 1].component) {
      return std::nullopt;
    }
  }
  const std::uint64_t domain = result.domain_weight();
  if (result.full_authority.domain_weight == 0) {
    result.full_authority.domain_weight = domain;
  }
  if (result.degraded_authority.domain_weight == 0) {
    result.degraded_authority.domain_weight = domain;
  }
  return result;
}

PartitionPolicy default_policy(PolicyGeneration generation) noexcept {
  PartitionPolicy policy;
  policy.id = PolicyId::from_validated("fabric.partition.default");
  policy.generation = generation;
  policy.schema_version = policy_schema_version;
  // A voter quorum is opt-in: with an empty weight table every component is a
  // non-voter, so a default min_voters above zero would be unsatisfiable and
  // would silently degrade every fabric.
  policy.full_authority.min_components = 3;
  policy.full_authority.min_voters = 0;
  policy.full_authority.min_weight = 0;
  policy.degraded_authority.min_components = 1;
  policy.max_evidence_age_ticks = 4096;
  return policy;
}

}  // namespace fabric_partition_manager
