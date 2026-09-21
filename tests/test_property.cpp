// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// Seeded property and differential testing. Every case is reproducible from its
// seed, and every case compares the library against the independent O(N^2)
// reference model in reference_model.hpp. Invariants are asserted after every
// generated operation, not only at the end.
#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "reference_model.hpp"

using namespace fpm_test;
namespace fpm = fabric_partition_manager;

namespace {

struct Scenario {
  fpm::ComponentRoster roster;
  std::vector<fpm::ReachabilityEvidence> evidence;
  fpm::PartitionPolicy policy;
  std::uint64_t now_tick = 0;
};

Scenario make_scenario(std::uint64_t seed) {
  fpm::DeterministicRandom random(seed);
  const std::uint32_t count = 2 + random.next_below(9);
  fpm::TopologyDefinition definition;
  definition.id = fpm::TopologyId::from_validated("property");
  definition.generation = fpm::TopologyGeneration::from_value(1);
  definition.provenance = fpm::Provenance::from_validated("property");
  for (std::uint32_t index = 0; index < count; ++index) {
    definition.components.push_back(component(index));
  }
  const auto roster = fpm::ComponentRoster::create(definition, test_limits());
  if (!roster.has_value()) {
    fail(__FILE__, __LINE__, "ComponentRoster::create", "the generated roster was rejected");
  }

  Scenario scenario;
  scenario.roster = *roster;
  scenario.now_tick = 1500;
  scenario.policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  scenario.policy.max_evidence_age_ticks = 200 + random.next_below(3000);

  const std::uint32_t bundle_count = random.next_below(4);
  for (std::uint32_t bundle_index = 0; bundle_index < bundle_count; ++bundle_index) {
    fpm::ReachabilityEvidence bundle;
    bundle.id = fpm::EvidenceId::from_validated("bundle-" + std::to_string(bundle_index));
    bundle.sequence = fpm::EvidenceSequence::from_value(bundle_index + 1);
    bundle.publisher = fpm::PublisherId::from_validated("publisher-" + std::to_string(bundle_index));
    bundle.publisher_boot = fpm::PublisherBootId::generate();
    bundle.topology_generation = definition.generation;
    bundle.generation = fpm::ReachabilityGeneration::from_value(bundle_index + 1);
    bundle.observed_at_tick = random.next_below(2500);
    bundle.validity_ticks = 1 + random.next_below(2500);
    bundle.provenance = fpm::Provenance::from_validated("property");
    const bool complete = random.next_below(4) == 0;
    bundle.completeness = complete ? fpm::EvidenceCompleteness::Complete
                                   : fpm::EvidenceCompleteness::Partial;
    for (std::uint32_t source = 0; source < count; ++source) {
      for (std::uint32_t target = 0; target < count; ++target) {
        if (source == target || random.next_below(100) >= 30) {
          continue;
        }
        fpm::LinkObservation observation;
        observation.source = component(source);
        observation.target = component(target);
        observation.value = random.next_bool() ? fpm::Reachability::Reachable
                                               : fpm::Reachability::Unreachable;
        bundle.observations.push_back(observation);
      }
    }
    if (complete) {
      for (std::uint32_t index = 0; index < count; ++index) {
        if (random.next_below(100) < 60) {
          bundle.covered_sources.push_back(component(index));
        }
      }
      if (bundle.covered_sources.empty()) {
        bundle.covered_sources.push_back(component(random.next_below(count)));
      }
    }
    scenario.evidence.push_back(std::move(bundle));
  }
  return scenario;
}

void assert_invariants(const fpm::ReachabilitySnapshot& snapshot, std::size_t component_count) {
  FPM_CHECK(snapshot.valid);
  FPM_EQ(snapshot.components.size(), component_count);
  FPM_EQ(snapshot.counters.ordered_total_classified(), snapshot.counters.ordered_pairs);
  FPM_EQ(snapshot.counters.ordered_pairs, static_cast<std::uint64_t>(component_count) *
                                              (component_count - 1));
  FPM_EQ(snapshot.counters.cross_component_ordered,
         snapshot.counters.cross_component_unreachable +
             snapshot.counters.cross_component_indeterminate);
  FPM_EQ(snapshot.counters.undirected_reachable,
         static_cast<std::uint64_t>(snapshot.edge_source.size()));
  FPM_EQ(snapshot.edge_source.size(), snapshot.edge_target.size());
  for (std::size_t index = 0; index < snapshot.edge_source.size(); ++index) {
    // Every edge lies inside a component, and edges are canonically oriented.
    FPM_CHECK(snapshot.edge_source[index] < snapshot.edge_target[index]);
    FPM_EQ(snapshot.component_of[snapshot.edge_source[index]],
           snapshot.component_of[snapshot.edge_target[index]]);
  }
  std::uint64_t total_members = 0;
  for (const std::uint32_t size : snapshot.component_sizes) {
    total_members += size;
  }
  FPM_EQ(total_members, static_cast<std::uint64_t>(component_count));
  for (const std::uint32_t representative : snapshot.component_representatives) {
    FPM_CHECK(snapshot.component_of[representative] == representative);
    FPM_CHECK(snapshot.component_sizes[representative] != 0);
  }
  // A confirmed decomposition must have no unresolved crossing pair, and an
  // unconfirmed one must have at least one.
  FPM_EQ(snapshot.confirmed, snapshot.counters.cross_component_indeterminate == 0);
}

void compare_with_reference(const Scenario& scenario) {
  fpm::ReachabilityBuildInput input;
  input.roster = &scenario.roster;
  input.evidence = &scenario.evidence;
  input.policy = &scenario.policy;
  input.now_tick = scenario.now_tick;
  const fpm::ReachabilitySnapshot snapshot =
      fpm::build_reachability_snapshot(input, test_limits());
  assert_invariants(snapshot, scenario.roster.size());

  const ReferenceOutcome reference =
      reference_resolve(scenario.roster, scenario.evidence, scenario.now_tick, scenario.policy);
  FPM_EQ(snapshot.counters.ordered_reachable, reference.counters.ordered_reachable);
  FPM_EQ(snapshot.counters.ordered_unreachable, reference.counters.ordered_unreachable);
  FPM_EQ(snapshot.counters.ordered_conflicting, reference.counters.ordered_conflicting);
  FPM_EQ(snapshot.counters.ordered_stale, reference.counters.ordered_stale);
  FPM_EQ(snapshot.counters.ordered_unknown, reference.counters.ordered_unknown);
  FPM_EQ(snapshot.counters.ordered_asymmetric, reference.counters.ordered_asymmetric);
  FPM_EQ(snapshot.counters.ordered_contradictory, reference.counters.ordered_contradictory);
  FPM_EQ(snapshot.counters.undirected_reachable, reference.counters.undirected_reachable);
  FPM_EQ(snapshot.counters.cross_component_ordered, reference.counters.cross_component_ordered);
  FPM_EQ(snapshot.counters.cross_component_unreachable,
         reference.counters.cross_component_unreachable);
  FPM_EQ(snapshot.confirmed, reference.confirmed);
  FPM_CHECK(snapshot.component_of == reference.component_of);
}

}  // namespace

FPM_TEST(property, differential_against_the_reference_model) {
  for (std::uint64_t seed = 1; seed <= 1000; ++seed) {
    const Scenario scenario = make_scenario(seed);
    compare_with_reference(scenario);
  }
}

FPM_TEST(property, permutation_invariance_of_evidence_and_observations) {
  for (std::uint64_t seed = 1001; seed <= 1200; ++seed) {
    const Scenario scenario = make_scenario(seed);
    std::mt19937 generator(static_cast<std::uint32_t>(seed));
    std::vector<fpm::ReachabilityEvidence> shuffled = scenario.evidence;
    for (fpm::ReachabilityEvidence& bundle : shuffled) {
      std::shuffle(bundle.observations.begin(), bundle.observations.end(), generator);
      std::shuffle(bundle.covered_sources.begin(), bundle.covered_sources.end(), generator);
    }
    std::shuffle(shuffled.begin(), shuffled.end(), generator);

    fpm::ReachabilityBuildInput first_input;
    first_input.roster = &scenario.roster;
    first_input.evidence = &scenario.evidence;
    first_input.policy = &scenario.policy;
    first_input.now_tick = scenario.now_tick;
    const fpm::ReachabilitySnapshot first =
        fpm::build_reachability_snapshot(first_input, test_limits());

    fpm::ReachabilityBuildInput second_input = first_input;
    second_input.evidence = &shuffled;
    const fpm::ReachabilitySnapshot second =
        fpm::build_reachability_snapshot(second_input, test_limits());

    FPM_EQ(first.evidence_digest, second.evidence_digest);
    FPM_EQ(first.edge_source, second.edge_source);
    FPM_EQ(first.edge_target, second.edge_target);
    FPM_EQ(first.component_of, second.component_of);
    FPM_EQ(first.confirmed, second.confirmed);
    FPM_EQ(first.counters.ordered_reachable, second.counters.ordered_reachable);
    FPM_EQ(first.counters.ordered_unreachable, second.counters.ordered_unreachable);
    FPM_EQ(first.counters.ordered_conflicting, second.counters.ordered_conflicting);
    FPM_EQ(first.counters.cross_component_unreachable, second.counters.cross_component_unreachable);
  }
}

FPM_TEST(property, partition_identity_is_stable_across_permutations) {
  for (std::uint64_t seed = 2001; seed <= 2100; ++seed) {
    fpm::DeterministicRandom random(seed);
    std::vector<std::uint32_t> groups;
    const std::uint32_t group_count = 2 + random.next_below(4);
    for (std::uint32_t index = 0; index < group_count; ++index) {
      groups.push_back(1 + random.next_below(4));
    }
    const fpm::SyntheticFabric built = fabric(groups, seed, 1, 1, true);
    const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
    FPM_CHECK(roster.has_value());
    const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));

    fpm::ReachabilityEvidence shuffled = built.evidence;
    std::mt19937 generator(static_cast<std::uint32_t>(seed * 31u + 7u));
    std::shuffle(shuffled.observations.begin(), shuffled.observations.end(), generator);

    const auto build = [&](const fpm::ReachabilityEvidence& evidence) {
      fpm::ReachabilityBuildInput input;
      input.roster = &roster.value();
      input.policy = &policy;
      input.now_tick = 0;
      std::vector<fpm::ReachabilityEvidence> single{evidence};
      input.evidence = &single;
      const fpm::ReachabilitySnapshot snapshot =
          fpm::build_reachability_snapshot(input, test_limits());
      fpm::PartitionBuildInput partitions_input;
      partitions_input.snapshot = &snapshot;
      partitions_input.generation = fpm::PartitionGeneration::from_value(3);
      partitions_input.policy = &policy;
      return fpm::build_partitions(partitions_input, test_limits());
    };

    const std::vector<fpm::Partition> first = build(built.evidence);
    const std::vector<fpm::Partition> second = build(shuffled);
    FPM_EQ(first.size(), second.size());
    FPM_EQ(first.size(), built.groups.size());
    for (std::size_t index = 0; index < first.size(); ++index) {
      FPM_EQ(first[index].id, second[index].id);
      FPM_EQ(first[index].membership, second[index].membership);
      FPM_EQ(first[index].lineage, second[index].lineage);
      FPM_EQ(first[index].members, built.groups[index]);
    }
  }
}

FPM_TEST(property, roster_permutation_does_not_change_partition_identity) {
  for (std::uint64_t seed = 3001; seed <= 3050; ++seed) {
    fpm::DeterministicRandom random(seed);
    const std::uint32_t count = 3 + random.next_below(8);
    fpm::TopologyDefinition definition;
    definition.id = fpm::TopologyId::from_validated("permuted");
    definition.generation = fpm::TopologyGeneration::from_value(1);
    definition.provenance = fpm::Provenance::from_validated("property");
    for (std::uint32_t index = 0; index < count; ++index) {
      definition.components.push_back(component(index));
    }
    fpm::TopologyDefinition permuted = definition;
    std::mt19937 generator(static_cast<std::uint32_t>(seed));
    std::shuffle(permuted.components.begin(), permuted.components.end(), generator);

    const auto first_roster = fpm::ComponentRoster::create(definition, test_limits());
    const auto second_roster = fpm::ComponentRoster::create(permuted, test_limits());
    FPM_CHECK(first_roster.has_value());
    FPM_CHECK(second_roster.has_value());
    FPM_EQ(first_roster->digest(), second_roster->digest());
    FPM_CHECK(first_roster->components() == second_roster->components());
  }
}
