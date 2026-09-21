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

fpm::ReachabilitySnapshot snapshot_of(const fpm::ComponentRoster& roster,
                                      const std::vector<fpm::ReachabilityEvidence>& evidence,
                                      const fpm::PartitionPolicy& policy, std::uint64_t now = 0) {
  fpm::ReachabilityBuildInput input;
  input.roster = &roster;
  input.evidence = &evidence;
  input.policy = &policy;
  input.now_tick = now;
  return fpm::build_reachability_snapshot(input, test_limits());
}

std::vector<fpm::Partition> partitions_of(const fpm::ReachabilitySnapshot& snapshot,
                                          fpm::PartitionGeneration generation,
                                          const fpm::PartitionPolicy& policy,
                                          const fpm::LineageStore* lineage = nullptr) {
  fpm::PartitionBuildInput input;
  input.snapshot = &snapshot;
  input.generation = generation;
  input.policy = &policy;
  input.lineage = lineage;
  return fpm::build_partitions(input, test_limits());
}

}  // namespace

FPM_TEST(partition, two_groups_produce_two_canonical_partitions) {
  const fpm::SyntheticFabric built = fabric({3, 2});
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  std::vector<fpm::ReachabilityEvidence> evidence{built.evidence};
  const fpm::ReachabilitySnapshot snapshot = snapshot_of(*roster, evidence, policy);
  FPM_CHECK(snapshot.confirmed);
  const std::vector<fpm::Partition> partitions =
      partitions_of(snapshot, fpm::PartitionGeneration::from_value(1), policy);
  FPM_EQ(partitions.size(), std::size_t{2});
  // Canonical order is lexicographic by member list, so the group whose first
  // member sorts first comes first: components 0..2 before components 3..4.
  FPM_EQ(partitions[0].member_count(), 3u);
  FPM_EQ(partitions[1].member_count(), 2u);
  FPM_CHECK(fpm::partition_canonical_less(partitions[0], partitions[1]));
  for (const fpm::Partition& partition : partitions) {
    FPM_CHECK(partition.membership == fpm::compute_membership_digest(partition.members));
    FPM_EQ(partition.certainty, fpm::PartitionCertainty::Confirmed);
    FPM_EQ(partition.lifecycle, fpm::PartitionLifecycle::Discovered);
    // The default policy configures no weights, so every component carries zero
    // weight and is not a voter: weight and voter quorums are opt-in.
    FPM_EQ(partition.total_weight, 0u);
    FPM_EQ(partition.voter_count, 0u);
    FPM_CHECK(!partition.authority.confers_authority());
    FPM_CHECK(partition.pending_merge_parents.empty());
  }
}

FPM_TEST(partition, identity_is_independent_of_observation_order) {
  const fpm::SyntheticFabric built = fabric({3, 3, 2}, 4242, 1, 1);
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));

  fpm::ReachabilityEvidence shuffled = built.evidence;
  std::mt19937 generator(99u);
  std::shuffle(shuffled.observations.begin(), shuffled.observations.end(), generator);

  const fpm::ReachabilitySnapshot first_snapshot =
      snapshot_of(*roster, std::vector<fpm::ReachabilityEvidence>{built.evidence}, policy);
  const fpm::ReachabilitySnapshot second_snapshot =
      snapshot_of(*roster, std::vector<fpm::ReachabilityEvidence>{shuffled}, policy);
  FPM_EQ(first_snapshot.evidence_digest, second_snapshot.evidence_digest);

  const std::vector<fpm::Partition> first =
      partitions_of(first_snapshot, fpm::PartitionGeneration::from_value(1), policy);
  const std::vector<fpm::Partition> second =
      partitions_of(second_snapshot, fpm::PartitionGeneration::from_value(1), policy);
  FPM_EQ(first.size(), second.size());
  for (std::size_t index = 0; index < first.size(); ++index) {
    FPM_EQ(first[index].id, second[index].id);
    FPM_EQ(first[index].membership, second[index].membership);
    FPM_EQ(first[index].members, second[index].members);
  }
}

FPM_TEST(partition, roster_edits_change_nothing_about_unrelated_partition_identity) {
  const fpm::SyntheticFabric built = fabric({2, 2, 2});
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  std::vector<fpm::ReachabilityEvidence> evidence{built.evidence};
  const fpm::ReachabilitySnapshot snapshot = snapshot_of(*roster, evidence, policy);
  const std::vector<fpm::Partition> partitions =
      partitions_of(snapshot, fpm::PartitionGeneration::from_value(5), policy);
  FPM_EQ(partitions.size(), std::size_t{3});
  for (std::size_t index = 0; index < partitions.size(); ++index) {
    const fpm::PartitionId expected = fpm::compute_partition_id(
        partitions[index].lineage, fpm::PartitionGeneration::from_value(5),
        partitions[index].membership);
    FPM_EQ(partitions[index].id, expected);
  }
  // A different generation yields different identities with the same lineage.
  const std::vector<fpm::Partition> later =
      partitions_of(snapshot, fpm::PartitionGeneration::from_value(6), policy);
  FPM_EQ(later.size(), partitions.size());
  for (std::size_t index = 0; index < later.size(); ++index) {
    FPM_EQ(later[index].lineage, partitions[index].lineage);
    FPM_NE(later[index].id, partitions[index].id);
  }
}

FPM_TEST(partition, component_computation_matches_the_reference_traversal) {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    const fpm::SyntheticFabric built = fabric({2, 3, 1, 4}, seed, 1, 1, false);
    const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
    FPM_CHECK(roster.has_value());
    const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
    std::vector<fpm::ReachabilityEvidence> evidence{built.evidence};
    const fpm::ReachabilitySnapshot snapshot = snapshot_of(*roster, evidence, policy);
    const ReferenceOutcome reference = reference_resolve(*roster, evidence, 0, policy);
    FPM_EQ(snapshot.component_count(), reference.component_members.size());
    FPM_CHECK(snapshot.component_of == reference.component_of);
    const std::vector<fpm::Partition> partitions =
        partitions_of(snapshot, fpm::PartitionGeneration::from_value(1), policy);
    FPM_EQ(partitions.size(), reference.component_members.size());
  }
}

FPM_TEST(partition, roster_rejects_duplicates_nil_and_order_dependent_definitions) {
  fpm::TopologyDefinition definition;
  definition.id = fpm::TopologyId::from_validated("topology");
  definition.generation = topology_generation(1);
  definition.provenance = fpm::Provenance::from_validated("test");
  definition.components = {component(0), component(1)};
  FPM_CHECK(fpm::ComponentRoster::create(definition, test_limits()).has_value());

  definition.components = {component(1), component(0)};
  const auto reordered = fpm::ComponentRoster::create(definition, test_limits());
  FPM_CHECK(reordered.has_value());
  FPM_EQ(reordered->components()[0], component(0));

  definition.components = {component(0), component(0)};
  FPM_CHECK(!fpm::ComponentRoster::create(definition, test_limits()).has_value());

  definition.components = {fpm::ComponentId{}, component(1)};
  FPM_CHECK(!fpm::ComponentRoster::create(definition, test_limits()).has_value());

  definition.components = {};
  const auto empty = fpm::ComponentRoster::create(definition, test_limits());
  FPM_CHECK(empty.has_value());
  FPM_EQ(empty->size(), std::size_t{0});
}

FPM_TEST(partition, empty_roster_is_never_a_confirmed_clean_fabric) {
  fpm::TopologyDefinition definition;
  definition.id = fpm::TopologyId::from_validated("empty");
  definition.generation = topology_generation(1);
  definition.provenance = fpm::Provenance::from_validated("test");
  const auto roster = fpm::ComponentRoster::create(definition, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  const fpm::ReachabilitySnapshot snapshot =
      snapshot_of(*roster, std::vector<fpm::ReachabilityEvidence>{}, policy);
  FPM_CHECK(snapshot.valid);
  FPM_CHECK(!snapshot.confirmed);
  FPM_EQ(snapshot.component_count(), std::size_t{0});
  const std::vector<fpm::Partition> partitions =
      partitions_of(snapshot, fpm::PartitionGeneration::from_value(1), policy);
  FPM_EQ(partitions.size(), std::size_t{0});
}

FPM_TEST(partition, single_connected_component_is_trivially_confirmed) {
  const fpm::SyntheticFabric built = fabric({5}, 3, 1, 1, false);
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  std::vector<fpm::ReachabilityEvidence> evidence{built.evidence};
  const fpm::ReachabilitySnapshot snapshot = snapshot_of(*roster, evidence, policy);
  FPM_EQ(snapshot.component_count(), std::size_t{1});
  FPM_EQ(snapshot.counters.cross_component_ordered, 0u);
  FPM_CHECK(snapshot.confirmed);
  const std::vector<fpm::Partition> partitions =
      partitions_of(snapshot, fpm::PartitionGeneration::from_value(1), policy);
  FPM_EQ(partitions.size(), std::size_t{1});
  FPM_EQ(partitions[0].certainty, fpm::PartitionCertainty::Confirmed);
}

FPM_TEST(partition, lineage_classification_detects_genesis_split_and_pending_merge) {
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));

  // Genesis: an empty lineage store yields a fresh lineage per component.
  const fpm::SyntheticFabric whole = fabric({4}, 5, 1, 1);
  auto whole_roster = fpm::ComponentRoster::create(whole.topology, test_limits());
  FPM_CHECK(whole_roster.has_value());
  std::vector<fpm::ReachabilityEvidence> whole_evidence{whole.evidence};
  const fpm::ReachabilitySnapshot whole_snapshot =
      snapshot_of(*whole_roster, whole_evidence, policy);
  const std::vector<fpm::Partition> whole_partitions =
      partitions_of(whole_snapshot, fpm::PartitionGeneration::from_value(1), policy);
  FPM_EQ(whole_partitions.size(), std::size_t{1});
  FPM_CHECK(whole_partitions[0].parent_lineage.is_nil());
  FPM_EQ(whole_partitions[0].lineage,
         fpm::genesis_lineage_id(whole_partitions[0].membership));

  // Record that lineage, then split the fabric into two groups.
  fpm::LineageStore store(test_limits());
  fpm::LineageRecord record;
  record.sequence = fpm::LineageSequence::from_value(1);
  record.lineage = whole_partitions[0].lineage;
  record.generation = fpm::PartitionGeneration::from_value(1);
  record.membership = whole_partitions[0].membership;
  record.members = whole_partitions[0].members;
  record.event = fpm::LineageEventKind::Genesis;
  record.epoch = fpm::CoordinatorEpoch::from_value(1);
  record.boot = fpm::CoordinatorBootId::generate();
  record.decision = fpm::DecisionId::from_validated("d1");
  record.digest = fpm::compute_lineage_digest(record);
  FPM_CHECK(store.append(record));

  const fpm::SyntheticFabric split = fabric({2, 2}, 5, 1, 2);
  auto split_roster = fpm::ComponentRoster::create(split.topology, test_limits());
  FPM_CHECK(split_roster.has_value());
  std::vector<fpm::ReachabilityEvidence> split_evidence{split.evidence};
  const fpm::ReachabilitySnapshot split_snapshot =
      snapshot_of(*split_roster, split_evidence, policy);
  const std::vector<fpm::Partition> split_partitions =
      partitions_of(split_snapshot, fpm::PartitionGeneration::from_value(2), policy, &store);
  FPM_EQ(split_partitions.size(), std::size_t{2});
  for (const fpm::Partition& partition : split_partitions) {
    FPM_EQ(partition.parent_lineage, whole_partitions[0].lineage);
    FPM_EQ(partition.lineage,
           fpm::split_lineage_id(whole_partitions[0].lineage, partition.membership));
    FPM_CHECK(partition.pending_merge_parents.empty());
  }

  // Now the fabric reconnects: one component spans both live child lineages.
  fpm::LineageStore split_store = store;
  for (const fpm::Partition& partition : split_partitions) {
    fpm::LineageRecord child;
    child.sequence = fpm::LineageSequence::from_value(
        split_store.highest_sequence().value() + 1);
    child.lineage = partition.lineage;
    child.generation = fpm::PartitionGeneration::from_value(2);
    child.membership = partition.membership;
    child.members = partition.members;
    child.event = fpm::LineageEventKind::Split;
    child.parent_left = partition.parent_lineage;
    child.epoch = fpm::CoordinatorEpoch::from_value(1);
    child.boot = fpm::CoordinatorBootId::generate();
    child.decision = fpm::DecisionId::from_validated("d2");
    child.digest = fpm::compute_lineage_digest(child);
    FPM_CHECK(split_store.append(child));
  }

  std::vector<fpm::ReachabilityEvidence> merged_evidence{whole.evidence};
  const fpm::ReachabilitySnapshot merged_snapshot =
      snapshot_of(*whole_roster, merged_evidence, policy);
  const std::vector<fpm::Partition> merged_partitions =
      partitions_of(merged_snapshot, fpm::PartitionGeneration::from_value(3), policy, &split_store);
  FPM_EQ(merged_partitions.size(), std::size_t{1});
  FPM_EQ(merged_partitions[0].pending_merge_parents.size(), std::size_t{2});
  FPM_CHECK(merged_partitions[0].parent_lineage.is_nil());
}

FPM_TEST(partition, canonical_order_is_a_total_order_with_no_ties) {
  const fpm::SyntheticFabric built = fabric({2, 3, 1, 4, 2}, 8, 1, 1);
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  std::vector<fpm::ReachabilityEvidence> evidence{built.evidence};
  const fpm::ReachabilitySnapshot snapshot = snapshot_of(*roster, evidence, policy);
  std::vector<fpm::Partition> partitions =
      partitions_of(snapshot, fpm::PartitionGeneration::from_value(1), policy);
  FPM_EQ(partitions.size(), std::size_t{5});
  for (std::size_t lhs = 0; lhs < partitions.size(); ++lhs) {
    for (std::size_t rhs = lhs + 1; rhs < partitions.size(); ++rhs) {
      FPM_CHECK(fpm::partition_canonical_less(partitions[lhs], partitions[rhs]));
      FPM_CHECK(!fpm::partition_canonical_less(partitions[rhs], partitions[lhs]));
    }
  }
  std::reverse(partitions.begin(), partitions.end());
  std::sort(partitions.begin(), partitions.end(), fpm::partition_canonical_less);
  for (std::size_t index = 1; index < partitions.size(); ++index) {
    FPM_CHECK(fpm::partition_canonical_less(partitions[index - 1], partitions[index]));
  }
}
