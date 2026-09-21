// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Shared fixture helpers for the test suites. Everything built here is
// SYNTHETIC: a fabricated roster with fabricated reachability observations.
#ifndef FABRIC_PARTITION_MANAGER_TESTS_FIXTURE_HPP
#define FABRIC_PARTITION_MANAGER_TESTS_FIXTURE_HPP

#include <algorithm>
#include <string>
#include <vector>

#include "fabric_partition_manager/fabric_partition_manager.hpp"
#include "test_harness.hpp"

namespace fpm_test {

namespace fpm = fabric_partition_manager;

inline fpm::Limits test_limits() { return fpm::default_limits(); }

inline fpm::ComponentId component(std::size_t index) {
  return fpm::synthetic_component_id(index);
}

inline fpm::TopologyGeneration topology_generation(std::uint64_t value) {
  return fpm::TopologyGeneration::from_value(value);
}

inline fpm::ReachabilityGeneration reachability_generation(std::uint64_t value) {
  return fpm::ReachabilityGeneration::from_value(value);
}

inline fpm::AttemptToken attempt(std::uint64_t sequence, const std::string& tag) {
  fpm::AttemptToken token;
  token.id = fpm::AttemptId::from_validated("a-" + tag + "-" + std::to_string(sequence));
  token.sequence = fpm::AttemptSequence::from_value(sequence);
  return token;
}

// A reproducible synthetic fabric with the requested group sizes.
inline fpm::SyntheticFabric fabric(const std::vector<std::uint32_t>& groups,
                                   std::uint64_t seed = 7,
                                   std::uint64_t topology = 1,
                                   std::uint64_t reachability = 1,
                                   bool complete = true) {
  fpm::SyntheticFabricOptions options;
  options.name = "test";
  options.group_sizes = groups;
  options.seed = seed;
  options.topology_generation = topology_generation(topology);
  options.reachability_generation = reachability_generation(reachability);
  options.complete_coverage = complete;
  options.observed_at_tick = 0;
  options.validity_ticks = 1000;
  const auto built = fpm::build_synthetic_fabric(options, test_limits());
  if (!built.has_value()) {
    fail(__FILE__, __LINE__, "build_synthetic_fabric", "the fixture could not be built");
  }
  return *built;
}

inline fpm::ReachabilityEvidence partial_evidence(const std::vector<fpm::ComponentId>& from,
                                                  const std::vector<fpm::ComponentId>& to,
                                                  const std::string& id) {
  fpm::ReachabilityEvidence evidence;
  evidence.id = fpm::EvidenceId::from_validated(id);
  evidence.sequence = fpm::EvidenceSequence::from_value(1);
  evidence.publisher = fpm::PublisherId::from_validated("publisher");
  evidence.publisher_boot = fpm::PublisherBootId::generate();
  evidence.topology_generation = topology_generation(1);
  evidence.generation = reachability_generation(1);
  evidence.observed_at_tick = 0;
  evidence.validity_ticks = 1000;
  evidence.completeness = fpm::EvidenceCompleteness::Partial;
  evidence.provenance = fpm::Provenance::from_validated("test");
  for (const fpm::ComponentId& source : from) {
    for (const fpm::ComponentId& target : to) {
      if (source == target) {
        continue;
      }
      fpm::LinkObservation forward;
      forward.source = source;
      forward.target = target;
      forward.value = fpm::Reachability::Reachable;
      fpm::LinkObservation reverse;
      reverse.source = target;
      reverse.target = source;
      reverse.value = fpm::Reachability::Reachable;
      evidence.observations.push_back(forward);
      evidence.observations.push_back(reverse);
    }
  }
  return evidence;
}

inline fpm::RuntimeOptions memory_runtime_options(fpm::PartitionPolicy policy) {
  fpm::RuntimeOptions options;
  options.store_directory.clear();
  options.provenance = fpm::Provenance::from_validated("test");
  options.limits = test_limits();
  options.policy = policy;
  options.durable = false;
  return options;
}

// A policy whose full quorum is a strict majority of the configured weights.
inline fpm::PartitionPolicy majority_policy(std::uint64_t generation) {
  fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(generation));
  policy.id = fpm::PolicyId::from_validated("test.majority");
  policy.full_authority = fpm::QuorumRequirement{};
  policy.full_authority.min_components = 1;
  policy.full_authority.require_strict_majority = true;
  policy.degraded_authority = fpm::QuorumRequirement{};
  policy.degraded_authority.min_components = 1;
  return policy;
}

// A policy whose full quorum is a plain component count, which is what makes a
// split-brain conflict reachable.
inline fpm::PartitionPolicy count_quorum_policy(std::uint64_t generation,
                                                std::uint32_t min_components) {
  fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(generation));
  policy.id = fpm::PolicyId::from_validated("test.count");
  policy.full_authority = fpm::QuorumRequirement{};
  policy.full_authority.min_components = min_components;
  policy.degraded_authority = fpm::QuorumRequirement{};
  policy.degraded_authority.min_components = 1;
  return policy;
}

inline const fpm::Partition* find_partition(const std::vector<fpm::Partition>& partitions,
                                            const fpm::ComponentId& member) {
  for (const fpm::Partition& partition : partitions) {
    if (std::find(partition.members.begin(), partition.members.end(), member) !=
        partition.members.end()) {
      return &partition;
    }
  }
  return nullptr;
}

inline std::uint64_t count_partitions_with_class(const std::vector<fpm::Partition>& partitions,
                                                 fpm::PartitionAuthorityClass klass) {
  std::uint64_t total = 0;
  for (const fpm::Partition& partition : partitions) {
    if (partition.authority.authority_class == klass) {
      ++total;
    }
  }
  return total;
}

}  // namespace fpm_test

#endif  // FABRIC_PARTITION_MANAGER_TESTS_FIXTURE_HPP
