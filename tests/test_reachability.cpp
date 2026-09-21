// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "reference_model.hpp"

using namespace fpm_test;
namespace fpm = fabric_partition_manager;

namespace {

fpm::ReachabilityBuildInput build_input(const fpm::ComponentRoster& roster,
                                       const std::vector<fpm::ReachabilityEvidence>& evidence,
                                       const fpm::PartitionPolicy& policy, std::uint64_t now) {
  fpm::ReachabilityBuildInput input;
  input.roster = &roster;
  input.evidence = &evidence;
  input.policy = &policy;
  input.now_tick = now;
  return input;
}

fpm::ComponentRoster roster_of(const std::vector<std::uint32_t>& groups) {
  const fpm::SyntheticFabric built = fabric(groups);
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  return *roster;
}

}  // namespace

FPM_TEST(reachability, direction_join_is_a_commutative_lattice) {
  using R = fpm::PairResolution;
  // The lattice is defined over the five inputs a direction can take.
  // Asymmetric is an output of the join, never an input to it.
  const std::vector<R> values = {R::Unknown, R::Reachable, R::Unreachable, R::Conflicting,
                                 R::Stale};
  for (const R lhs : values) {
    for (const R rhs : values) {
      FPM_EQ(fpm::join_directions(lhs, rhs), fpm::join_directions(rhs, lhs));
      FPM_EQ(fpm::join_directions(lhs, lhs), lhs);
    }
  }
  FPM_EQ(fpm::join_directions(R::Reachable, R::Reachable), R::Reachable);
  FPM_EQ(fpm::join_directions(R::Unreachable, R::Unreachable), R::Unreachable);
  FPM_EQ(fpm::join_directions(R::Reachable, R::Unreachable), R::Conflicting);
  FPM_EQ(fpm::join_directions(R::Reachable, R::Unknown), R::Asymmetric);
  FPM_EQ(fpm::join_directions(R::Unreachable, R::Unknown), R::Asymmetric);
  FPM_EQ(fpm::join_directions(R::Unknown, R::Unknown), R::Unknown);
  FPM_EQ(fpm::join_directions(R::Stale, R::Unknown), R::Unknown);
}

FPM_TEST(reachability, absent_evidence_is_unknown_and_never_clean) {
  const fpm::ComponentRoster roster = roster_of({2, 2});
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  const std::vector<fpm::ReachabilityEvidence> none;
  const fpm::ReachabilitySnapshot snapshot =
      fpm::build_reachability_snapshot(build_input(roster, none, policy, 0), test_limits());
  FPM_CHECK(snapshot.valid);
  FPM_EQ(snapshot.component_count(), std::size_t{4});
  FPM_CHECK(!snapshot.confirmed);
  FPM_EQ(snapshot.counters.ordered_reachable, 0u);
  FPM_EQ(snapshot.counters.ordered_unreachable, 0u);
  FPM_EQ(snapshot.counters.ordered_unknown, 12u);
  FPM_EQ(snapshot.counters.cross_component_indeterminate, 12u);
  FPM_EQ(snapshot.edge_source.size(), std::size_t{0});
}

FPM_TEST(reachability, complete_coverage_confirms_a_two_way_split) {
  const fpm::SyntheticFabric built = fabric({2, 2});
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  std::vector<fpm::ReachabilityEvidence> evidence{built.evidence};
  const fpm::ReachabilitySnapshot snapshot =
      fpm::build_reachability_snapshot(build_input(*roster, evidence, policy, 0), test_limits());
  FPM_CHECK(snapshot.confirmed);
  FPM_EQ(snapshot.component_count(), std::size_t{2});
  FPM_EQ(snapshot.counters.cross_component_ordered, 8u);
  FPM_EQ(snapshot.counters.cross_component_unreachable, 8u);
  FPM_EQ(snapshot.counters.cross_component_indeterminate, 0u);
  FPM_EQ(snapshot.counters.ordered_reachable, 4u);
  FPM_EQ(snapshot.counters.ordered_unknown, 0u);
}

FPM_TEST(reachability, partial_coverage_can_never_confirm) {
  const fpm::SyntheticFabric built = fabric({2, 2}, 7, 1, 1, false);
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  std::vector<fpm::ReachabilityEvidence> evidence{built.evidence};
  const fpm::ReachabilitySnapshot snapshot =
      fpm::build_reachability_snapshot(build_input(*roster, evidence, policy, 0), test_limits());
  FPM_CHECK(!snapshot.confirmed);
  FPM_CHECK(snapshot.counters.cross_component_indeterminate > 0);
  // The discovered decomposition is still the refinement the evidence supports.
  FPM_EQ(snapshot.component_count(), std::size_t{2});
}

FPM_TEST(reachability, asymmetric_evidence_is_surfaced_not_assumed) {
  const fpm::ComponentRoster roster = roster_of({1, 1});
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  fpm::ReachabilityEvidence bundle;
  bundle.id = fpm::EvidenceId::from_validated("asymmetric");
  bundle.sequence = fpm::EvidenceSequence::from_value(1);
  bundle.publisher = fpm::PublisherId::from_validated("publisher");
  bundle.publisher_boot = fpm::PublisherBootId::generate();
  bundle.topology_generation = topology_generation(1);
  bundle.generation = reachability_generation(1);
  bundle.validity_ticks = 1000;
  bundle.completeness = fpm::EvidenceCompleteness::Partial;
  bundle.provenance = fpm::Provenance::from_validated("test");
  fpm::LinkObservation observation;
  observation.source = component(0);
  observation.target = component(1);
  observation.value = fpm::Reachability::Reachable;
  bundle.observations.push_back(observation);
  std::vector<fpm::ReachabilityEvidence> evidence{bundle};
  const fpm::ReachabilitySnapshot snapshot =
      fpm::build_reachability_snapshot(build_input(roster, evidence, policy, 0), test_limits());
  // One direction is proven and the reverse was never observed: no edge exists.
  FPM_EQ(snapshot.edge_source.size(), std::size_t{0});
  FPM_EQ(snapshot.component_count(), std::size_t{2});
  // Exactly one ordered pair carries a determination whose reverse is unknown.
  FPM_EQ(snapshot.counters.ordered_asymmetric, 1u);
  FPM_EQ(snapshot.counters.ordered_reachable, 1u);
  FPM_EQ(snapshot.counters.ordered_unknown, 1u);
  FPM_CHECK(!snapshot.confirmed);
}

FPM_TEST(reachability, contradictory_directions_are_conflicting) {
  const fpm::ComponentRoster roster = roster_of({1, 1});
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  fpm::ReachabilityEvidence bundle;
  bundle.id = fpm::EvidenceId::from_validated("contradiction");
  bundle.sequence = fpm::EvidenceSequence::from_value(1);
  bundle.publisher = fpm::PublisherId::from_validated("publisher");
  bundle.publisher_boot = fpm::PublisherBootId::generate();
  bundle.topology_generation = topology_generation(1);
  bundle.generation = reachability_generation(1);
  bundle.validity_ticks = 1000;
  bundle.completeness = fpm::EvidenceCompleteness::Partial;
  bundle.provenance = fpm::Provenance::from_validated("test");
  fpm::LinkObservation forward;
  forward.source = component(0);
  forward.target = component(1);
  forward.value = fpm::Reachability::Reachable;
  fpm::LinkObservation reverse;
  reverse.source = component(1);
  reverse.target = component(0);
  reverse.value = fpm::Reachability::Unreachable;
  bundle.observations.push_back(forward);
  bundle.observations.push_back(reverse);
  std::vector<fpm::ReachabilityEvidence> evidence{bundle};
  const fpm::ReachabilitySnapshot snapshot =
      fpm::build_reachability_snapshot(build_input(roster, evidence, policy, 0), test_limits());
  FPM_EQ(snapshot.edge_source.size(), std::size_t{0});
  FPM_EQ(snapshot.counters.ordered_contradictory, 1u);
  FPM_EQ(snapshot.counters.ordered_reachable, 1u);
  FPM_EQ(snapshot.counters.ordered_unreachable, 1u);
  FPM_CHECK(!snapshot.confirmed);
}

FPM_TEST(reachability, two_complete_claims_about_one_source_conflict) {
  const fpm::ComponentRoster roster = roster_of({2});
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));

  // Two fresh complete claims about the same source that disagree about one
  // pair: the first is silent, which asserts unreachable, and the second lists
  // the pair as reachable.
  const auto claim = [](const std::string& id, std::uint64_t sequence, bool observe) {
    fpm::ReachabilityEvidence bundle;
    bundle.id = fpm::EvidenceId::from_validated(id);
    bundle.sequence = fpm::EvidenceSequence::from_value(sequence);
    bundle.publisher = fpm::PublisherId::from_validated("publisher-" + id);
    bundle.publisher_boot = fpm::PublisherBootId::generate();
    bundle.topology_generation = topology_generation(1);
    bundle.generation = reachability_generation(1);
    bundle.validity_ticks = 1000;
    bundle.completeness = fpm::EvidenceCompleteness::Complete;
    bundle.provenance = fpm::Provenance::from_validated("test");
    bundle.covered_sources = {component(0)};
    if (observe) {
      fpm::LinkObservation observation;
      observation.source = component(0);
      observation.target = component(1);
      observation.value = fpm::Reachability::Reachable;
      bundle.observations.push_back(observation);
    }
    return bundle;
  };

  std::vector<fpm::ReachabilityEvidence> evidence{claim("silent", 1, false),
                                                  claim("reachable", 2, true)};
  const fpm::ReachabilitySnapshot snapshot =
      fpm::build_reachability_snapshot(build_input(roster, evidence, policy, 0), test_limits());
  FPM_CHECK(snapshot.complete_coverage_conflict);
  FPM_CHECK(!snapshot.confirmed);
  // The single ordered pair sourced at component 0 is contradictory.
  FPM_EQ(snapshot.counters.ordered_conflicting, 1u);
  FPM_EQ(snapshot.edge_source.size(), std::size_t{0});

  // Two claims that agree are not a conflict: repeating the same complete
  // observation keeps the pair reachable in both directions.
  std::vector<fpm::ReachabilityEvidence> agreeing{claim("a", 1, true), claim("b", 2, true)};
  fpm::ReachabilityEvidence reverse = agreeing[0];
  reverse.id = fpm::EvidenceId::from_validated("c");
  reverse.sequence = fpm::EvidenceSequence::from_value(3);
  reverse.publisher = fpm::PublisherId::from_validated("publisher-c");
  fpm::LinkObservation reverse_link;
  reverse_link.source = component(1);
  reverse_link.target = component(0);
  reverse_link.value = fpm::Reachability::Reachable;
  reverse.observations.push_back(reverse_link);
  reverse.covered_sources = {component(0), component(1)};
  fpm::ReachabilityEvidence forward = agreeing[1];
  forward.id = fpm::EvidenceId::from_validated("d");
  forward.sequence = fpm::EvidenceSequence::from_value(4);
  forward.publisher = fpm::PublisherId::from_validated("publisher-d");
  forward.observations.push_back(reverse_link);
  forward.covered_sources = {component(0), component(1)};
  std::vector<fpm::ReachabilityEvidence> agreed{reverse, forward};
  const fpm::ReachabilitySnapshot agreed_snapshot =
      fpm::build_reachability_snapshot(build_input(roster, agreed, policy, 0), test_limits());
  FPM_EQ(agreed_snapshot.counters.ordered_conflicting, 0u);
  FPM_EQ(agreed_snapshot.counters.ordered_reachable, 2u);
  FPM_EQ(agreed_snapshot.edge_source.size(), std::size_t{1});
  FPM_CHECK(agreed_snapshot.confirmed);
}

FPM_TEST(reachability, stale_evidence_does_not_confirm_and_is_reported_stale) {
  const fpm::SyntheticFabric built = fabric({1, 1});
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  policy.max_evidence_age_ticks = 100;
  std::vector<fpm::ReachabilityEvidence> evidence{built.evidence};
  const fpm::ReachabilitySnapshot fresh =
      fpm::build_reachability_snapshot(build_input(*roster, evidence, policy, 10), test_limits());
  FPM_CHECK(fresh.any_evidence_fresh);
  const fpm::ReachabilitySnapshot aged =
      fpm::build_reachability_snapshot(build_input(*roster, evidence, policy, 5000), test_limits());
  FPM_CHECK(!aged.any_evidence_fresh);
  FPM_EQ(aged.fresh_bundle_count, std::size_t{0});
  FPM_EQ(aged.stale_bundle_count, std::size_t{1});
  FPM_CHECK(!aged.confirmed);
  FPM_EQ(aged.counters.ordered_unknown, 2u);
}

FPM_TEST(reachability, observation_order_does_not_change_the_result) {
  const fpm::SyntheticFabric built = fabric({3, 2, 2});
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));

  fpm::ReachabilityEvidence shuffled = built.evidence;
  std::mt19937 generator(20260101u);
  std::shuffle(shuffled.observations.begin(), shuffled.observations.end(), generator);
  std::shuffle(shuffled.covered_sources.begin(), shuffled.covered_sources.end(), generator);

  std::vector<fpm::ReachabilityEvidence> first{built.evidence};
  std::vector<fpm::ReachabilityEvidence> second{shuffled};
  const fpm::ReachabilitySnapshot a =
      fpm::build_reachability_snapshot(build_input(*roster, first, policy, 0), test_limits());
  const fpm::ReachabilitySnapshot b =
      fpm::build_reachability_snapshot(build_input(*roster, second, policy, 0), test_limits());
  FPM_EQ(a.evidence_digest, b.evidence_digest);
  FPM_EQ(a.edge_source, b.edge_source);
  FPM_EQ(a.edge_target, b.edge_target);
  FPM_EQ(a.component_of, b.component_of);
  FPM_EQ(a.counters.ordered_reachable, b.counters.ordered_reachable);
  FPM_EQ(a.counters.ordered_unreachable, b.counters.ordered_unreachable);
  FPM_EQ(a.counters.cross_component_unreachable, b.counters.cross_component_unreachable);
  FPM_EQ(a.confirmed, b.confirmed);
}

FPM_TEST(reachability, counters_match_the_independent_reference_model) {
  const fpm::ComponentRoster roster = roster_of({2, 3, 2});
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  const fpm::SyntheticFabric built = fabric({2, 3, 2});
  std::vector<fpm::ReachabilityEvidence> evidence{built.evidence};
  const fpm::ReachabilitySnapshot snapshot =
      fpm::build_reachability_snapshot(build_input(roster, evidence, policy, 0), test_limits());
  const ReferenceOutcome reference = reference_resolve(roster, evidence, 0, policy);
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
  FPM_EQ(snapshot.component_count(), reference.component_members.size());
}

FPM_TEST(reachability, ordered_categories_sum_to_the_ordered_pair_count) {
  const fpm::ComponentRoster roster = roster_of({2, 2, 2});
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  const fpm::SyntheticFabric built = fabric({2, 2, 2}, 11, 1, 1, false);
  std::vector<fpm::ReachabilityEvidence> evidence{built.evidence};
  const fpm::ReachabilitySnapshot snapshot =
      fpm::build_reachability_snapshot(build_input(roster, evidence, policy, 0), test_limits());
  FPM_EQ(snapshot.counters.ordered_total_classified(), snapshot.counters.ordered_pairs);
  FPM_EQ(snapshot.counters.ordered_pairs, 30u);
  FPM_EQ(snapshot.counters.cross_component_ordered,
         snapshot.counters.cross_component_unreachable +
             snapshot.counters.cross_component_indeterminate);
}

FPM_TEST(reachability, ledger_rejects_future_dated_and_zero_validity_evidence) {
  const fpm::SyntheticFabric built = fabric({2});
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  fpm::EvidenceLedger ledger(test_limits());

  fpm::ReachabilityEvidence future = built.evidence;
  future.observed_at_tick = 10'000;
  const fpm::EvidenceAcceptance future_result =
      ledger.accept(future, *roster, policy, 100);
  FPM_EQ(future_result.status, fpm::EvidenceAcceptanceStatus::RejectedFutureDated);

  fpm::ReachabilityEvidence zero = built.evidence;
  zero.validity_ticks = 0;
  const fpm::EvidenceAcceptance zero_result = ledger.accept(zero, *roster, policy, 0);
  FPM_EQ(zero_result.status, fpm::EvidenceAcceptanceStatus::RejectedMalformed);

  const fpm::EvidenceAcceptance accepted = ledger.accept(built.evidence, *roster, policy, 0);
  FPM_EQ(accepted.status, fpm::EvidenceAcceptanceStatus::Accepted);
  FPM_EQ(ledger.retained_count(), std::size_t{1});

  const fpm::EvidenceAcceptance replay = ledger.accept(built.evidence, *roster, policy, 0);
  FPM_EQ(replay.status, fpm::EvidenceAcceptanceStatus::RejectedDuplicate);
}

FPM_TEST(reachability, ledger_fences_retired_publisher_incarnations) {
  const fpm::SyntheticFabric built = fabric({2});
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  fpm::EvidenceLedger ledger(test_limits());

  fpm::ReachabilityEvidence first = built.evidence;
  FPM_EQ(ledger.accept(first, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::Accepted);

  fpm::ReachabilityEvidence reincarnated = built.evidence;
  reincarnated.publisher_boot = fpm::PublisherBootId::generate();
  reincarnated.sequence = fpm::EvidenceSequence::from_value(1);
  FPM_EQ(ledger.accept(reincarnated, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::Accepted);

  // The first incarnation is now retired: a replay from it must be refused.
  fpm::ReachabilityEvidence replay = first;
  replay.sequence = fpm::EvidenceSequence::from_value(2);
  FPM_EQ(ledger.accept(replay, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::RejectedRetiredBoot);

  fpm::ReachabilityEvidence regressed = reincarnated;
  regressed.sequence = fpm::EvidenceSequence::from_value(0);
  FPM_EQ(ledger.accept(regressed, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::RejectedRegression);
}

FPM_TEST(reachability, ledger_rejects_evidence_naming_a_foreign_component) {
  const fpm::SyntheticFabric built = fabric({2});
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  fpm::EvidenceLedger ledger(test_limits());
  fpm::ReachabilityEvidence foreign = built.evidence;
  fpm::LinkObservation observation;
  observation.source = component(0);
  observation.target = fpm::ComponentId::from_validated("node-not-in-roster");
  observation.value = fpm::Reachability::Reachable;
  foreign.observations.push_back(observation);
  const fpm::EvidenceAcceptance result = ledger.accept(foreign, *roster, policy, 0);
  FPM_EQ(result.status, fpm::EvidenceAcceptanceStatus::RejectedUnknownComponent);
}

FPM_TEST(reachability, ledger_rejects_explicit_unknown_observations_and_self_links) {
  const fpm::SyntheticFabric built = fabric({2});
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  fpm::EvidenceLedger ledger(test_limits());

  fpm::ReachabilityEvidence unknown = built.evidence;
  fpm::LinkObservation observation;
  observation.source = component(0);
  observation.target = component(1);
  observation.value = fpm::Reachability::Unknown;
  unknown.observations.push_back(observation);
  FPM_EQ(ledger.accept(unknown, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::RejectedMalformed);

  fpm::ReachabilityEvidence self = built.evidence;
  fpm::LinkObservation loop;
  loop.source = component(0);
  loop.target = component(0);
  loop.value = fpm::Reachability::Reachable;
  self.observations.push_back(loop);
  FPM_EQ(ledger.accept(self, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::RejectedMalformed);
}

FPM_TEST(reachability, ledger_rejects_partial_bundles_that_claim_coverage) {
  const fpm::SyntheticFabric built = fabric({2});
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  fpm::EvidenceLedger ledger(test_limits());
  fpm::ReachabilityEvidence partial = built.evidence;
  partial.completeness = fpm::EvidenceCompleteness::Partial;
  FPM_EQ(ledger.accept(partial, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::RejectedMalformed);

  fpm::ReachabilityEvidence complete = built.evidence;
  complete.covered_sources.clear();
  FPM_EQ(ledger.accept(complete, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::RejectedMalformed);

  fpm::ReachabilityEvidence duplicated = built.evidence;
  duplicated.covered_sources = {component(0), component(0)};
  FPM_EQ(ledger.accept(duplicated, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::RejectedMalformed);
}

FPM_TEST(reachability, ledger_rejects_generation_mismatch_and_empty_bundles) {
  const fpm::SyntheticFabric built = fabric({2});
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  fpm::EvidenceLedger ledger(test_limits());

  fpm::ReachabilityEvidence old = built.evidence;
  old.topology_generation = topology_generation(0);
  FPM_EQ(ledger.accept(old, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::RejectedGeneration);

  fpm::ReachabilityEvidence empty = built.evidence;
  empty.observations.clear();
  empty.covered_sources.clear();
  FPM_EQ(ledger.accept(empty, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::RejectedEmpty);
}
