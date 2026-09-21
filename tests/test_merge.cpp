// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "fixture.hpp"

using namespace fpm_test;
namespace fpm = fabric_partition_manager;

namespace {

struct SplitMergeHarness {
  SplitMergeHarness() : runtime(std::make_unique<fpm::PartitionRuntime>(memory_runtime_options(
                            count_quorum_policy(1, 3)))) {
    whole = fabric({4}, 5, 1, 1, true);
    split = fabric({2, 2}, 5, 1, 2, true);
    (void)runtime->adopt_topology(whole.topology);
  }

  fpm::ReachabilityEvidence timed(const fpm::SyntheticFabric& source) {
    fpm::ReachabilityEvidence evidence = source.evidence;
    evidence.observed_at_tick = tick;
    evidence.validity_ticks = 5;
    evidence.publisher_boot = fpm::PublisherBootId::generate();
    return evidence;
  }

  void advance() {
    tick += 10;
    FPM_CHECK(runtime->advance_ticks(10));
  }

  std::unique_ptr<fpm::PartitionRuntime> runtime;
  fpm::SyntheticFabric whole;
  fpm::SyntheticFabric split;
  std::uint64_t tick = 0;
};

}  // namespace

FPM_TEST(merge, reconnect_is_a_governed_event_not_a_set_union) {
  SplitMergeHarness harness;

  // Step 1: the whole fabric is one component and receives authority.
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.whole)).verdict ==
            fpm::DecisionVerdict::Granted);
  fpm::PartitionAssessment first = harness.runtime->assess();
  FPM_EQ(first.partitions.size(), std::size_t{1});
  FPM_EQ(first.partitions[0].authority.authority_class, fpm::PartitionAuthorityClass::Primary);
  const fpm::LineageId genesis = first.partitions[0].lineage;
  harness.advance();

  // Step 2: the fabric splits into two components, each a child of the genesis
  // lineage.
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.split)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment second = harness.runtime->assess();
  FPM_EQ(second.partitions.size(), std::size_t{2});
  std::vector<fpm::LineageId> children;
  for (const fpm::Partition& partition : second.partitions) {
    FPM_EQ(partition.parent_lineage, genesis);
    FPM_CHECK(partition.pending_merge_parents.empty());
    children.push_back(partition.lineage);
  }
  std::sort(children.begin(), children.end());
  harness.advance();

  // Step 3: the fabric reconnects. The component set now spans two live
  // lineages, and no authority is granted until the merge is governed.
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.whole)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment third = harness.runtime->assess();
  FPM_EQ(third.partitions.size(), std::size_t{1});
  const fpm::Partition& reconnected = third.partitions[0];
  FPM_EQ(reconnected.pending_merge_parents.size(), std::size_t{2});
  FPM_CHECK(reconnected.pending_merge_parents == children);
  FPM_EQ(reconnected.authority.authority_class, fpm::PartitionAuthorityClass::ObserveOnly);
  FPM_CHECK(reconnected.authority.authorized.contains(fpm::AuthorityCapability::Observe));
  FPM_CHECK(!reconnected.authority.authorized.contains(fpm::AuthorityCapability::ServeWrites));
  FPM_EQ(reconnected.lifecycle, fpm::PartitionLifecycle::Discovered);
  FPM_CHECK(has_reason_code(reconnected.reasons, fpm::ReasonCode::MergeRejectedLineageConflict));

  // Step 4: an ungoverned request naming a non-parent lineage is refused.
  fpm::MergeRequest bogus;
  bogus.attempt = attempt(1, "bogus");
  bogus.expected_epoch = harness.runtime->epoch();
  bogus.subject = reconnected.id;
  bogus.left = children[0];
  bogus.right = fpm::LineageId::from_validated("lffffffffffffffffffffffffffffffff");
  fpm::MergeOutcome refused = harness.runtime->request_merge(bogus);
  FPM_EQ(refused.verdict, fpm::DecisionVerdict::Invalid);

  // Step 5: two sequential governed pairwise reconciliations are required, one
  // per parent pair. The first commits and the partition keeps one pending
  // parent; the second resolves it.
  fpm::MergeRequest request;
  request.attempt = attempt(2, "merge");
  request.expected_epoch = harness.runtime->epoch();
  request.subject = reconnected.id;
  request.left = children[0];
  request.right = children[1];
  fpm::MergeOutcome merged = harness.runtime->request_merge(request);
  FPM_EQ(merged.verdict, fpm::DecisionVerdict::Granted);
  FPM_EQ(merged.merged_lineage, fpm::merge_lineage_id(children[0], children[1]));
  FPM_CHECK(!merged.merged_partition.is_nil());
  FPM_CHECK(!merged.decision.fences_issued.empty());

  const fpm::PartitionAssessment final_assessment = harness.runtime->last_assessment();
  FPM_EQ(final_assessment.partitions.size(), std::size_t{1});
  FPM_CHECK(final_assessment.partitions[0].pending_merge_parents.empty());
  FPM_EQ(final_assessment.partitions[0].lineage, merged.merged_lineage);
  FPM_EQ(final_assessment.partitions[0].authority.authority_class,
         fpm::PartitionAuthorityClass::Primary);
  FPM_EQ(final_assessment.decision.verdict, fpm::DecisionVerdict::Granted);

  // The parents are superseded by the merge record and are no longer live.
  const std::vector<fpm::LineageId> live = [&]() {
    std::vector<fpm::LineageId> values;
    for (const fpm::LineageRecord& record : harness.runtime->lineage_records()) {
      values.push_back(record.lineage);
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
  }();
  FPM_CHECK(std::find(live.begin(), live.end(), merged.merged_lineage) != live.end());
}

FPM_TEST(merge, conflicting_lineage_parents_are_refused) {
  SplitMergeHarness harness;
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.whole)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment first = harness.runtime->assess();
  const fpm::LineageId genesis = first.partitions[0].lineage;
  harness.advance();
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.split)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment second = harness.runtime->assess();
  harness.advance();
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.whole)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment third = harness.runtime->assess();
  const fpm::PartitionId subject = third.partitions[0].id;

  // Naming the genesis lineage and one of its children is an ancestry conflict:
  // one already supersedes the other.
  fpm::MergeRequest conflict;
  conflict.attempt = attempt(1, "conflict");
  conflict.expected_epoch = harness.runtime->epoch();
  conflict.subject = subject;
  conflict.left = genesis;
  conflict.right = second.partitions[0].lineage;
  const fpm::MergeOutcome outcome = harness.runtime->request_merge(conflict);
  FPM_EQ(outcome.verdict, fpm::DecisionVerdict::Invalid);
}

FPM_TEST(merge, epoch_and_generation_binding_are_enforced) {
  SplitMergeHarness harness;
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.whole)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment first = harness.runtime->assess();
  harness.advance();
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.split)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment second = harness.runtime->assess();
  harness.advance();
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.whole)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment third = harness.runtime->assess();

  fpm::MergeRequest stale_epoch;
  stale_epoch.attempt = attempt(1, "stale-epoch");
  stale_epoch.expected_epoch = fpm::CoordinatorEpoch::from_value(
      harness.runtime->epoch().value() + 3);
  stale_epoch.subject = third.partitions[0].id;
  stale_epoch.left = second.partitions[0].lineage;
  stale_epoch.right = second.partitions[1].lineage;
  const fpm::MergeOutcome epoch_outcome = harness.runtime->request_merge(stale_epoch);
  FPM_EQ(epoch_outcome.verdict, fpm::DecisionVerdict::Denied);
  FPM_CHECK(has_reason_code(epoch_outcome.decision.reasons,
                            fpm::ReasonCode::EpochMismatchReason));

  fpm::MergeRequest unknown;
  unknown.attempt = attempt(2, "unknown");
  unknown.expected_epoch = harness.runtime->epoch();
  unknown.subject = first.partitions[0].id;
  unknown.left = second.partitions[0].lineage;
  unknown.right = second.partitions[1].lineage;
  const fpm::MergeOutcome unknown_outcome = harness.runtime->request_merge(unknown);
  FPM_EQ(unknown_outcome.verdict, fpm::DecisionVerdict::Denied);
  FPM_CHECK(has_reason_code(unknown_outcome.decision.reasons,
                            fpm::ReasonCode::MergeRejectedUnknownSubject));
}

FPM_TEST(merge, merge_requires_confirmed_and_fresh_evidence) {
  SplitMergeHarness harness;
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.whole)).verdict ==
            fpm::DecisionVerdict::Granted);
  (void)harness.runtime->assess();
  harness.advance();
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.split)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment second = harness.runtime->assess();
  harness.advance();
  // Partial evidence that reconnects components 0, 1 and 2 while leaving the
  // links to component 3 unobserved. The component set spans both live child
  // lineages, so a governed merge is required, and the decomposition is not
  // confirmed because the crossing pairs to component 3 are unknown.
  fpm::ReachabilityEvidence partial;
  partial.id = fpm::EvidenceId::from_validated("partial-reconnect");
  partial.sequence = fpm::EvidenceSequence::from_value(1);
  partial.publisher = fpm::PublisherId::from_validated("publisher");
  partial.publisher_boot = fpm::PublisherBootId::generate();
  partial.topology_generation = topology_generation(1);
  partial.generation = reachability_generation(3);
  partial.observed_at_tick = harness.tick;
  partial.validity_ticks = 5;
  partial.completeness = fpm::EvidenceCompleteness::Partial;
  partial.provenance = fpm::Provenance::from_validated("test");
  const auto link = [&partial](std::size_t lhs, std::size_t rhs) {
    fpm::LinkObservation forward;
    forward.source = component(lhs);
    forward.target = component(rhs);
    forward.value = fpm::Reachability::Reachable;
    fpm::LinkObservation reverse;
    reverse.source = component(rhs);
    reverse.target = component(lhs);
    reverse.value = fpm::Reachability::Reachable;
    partial.observations.push_back(forward);
    partial.observations.push_back(reverse);
  };
  link(0, 1);
  link(1, 2);
  FPM_CHECK(harness.runtime->ingest_evidence(partial).verdict == fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment third = harness.runtime->assess();
  FPM_CHECK(!third.confirmed);
  FPM_EQ(third.partitions.size(), std::size_t{2});
  const fpm::Partition* reconnected = find_partition(third.partitions, component(0));
  if (reconnected == nullptr) {
    fail(__FILE__, __LINE__, "find_partition", "the reconnected component set must be present");
    return;
  }
  FPM_EQ(reconnected->member_count(), 3u);
  FPM_EQ(reconnected->pending_merge_parents.size(), std::size_t{2});

  fpm::MergeRequest request;
  request.attempt = attempt(1, "unconfirmed");
  request.expected_epoch = harness.runtime->epoch();
  request.subject = reconnected->id;
  request.left = second.partitions[0].lineage;
  request.right = second.partitions[1].lineage;
  const fpm::MergeOutcome outcome = harness.runtime->request_merge(request);
  // A merge is refused while the decomposition that motivates it is not
  // confirmed by fresh evidence.
  FPM_EQ(outcome.verdict, fpm::DecisionVerdict::Denied);
  FPM_CHECK(has_reason_code(outcome.decision.reasons, fpm::ReasonCode::MergeRejectedNotReachable));
}

FPM_TEST(merge, split_merge_split_fences_authority_at_every_step) {
  SplitMergeHarness harness;
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.whole)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment a = harness.runtime->assess();
  const fpm::LineageId genesis = a.partitions[0].lineage;
  harness.advance();

  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.split)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment b = harness.runtime->assess();
  FPM_EQ(b.partitions.size(), std::size_t{2});
  harness.advance();

  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.whole)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment c = harness.runtime->assess();
  FPM_EQ(c.partitions.size(), std::size_t{1});

  fpm::MergeRequest request;
  request.attempt = attempt(1, "reconcile");
  request.expected_epoch = harness.runtime->epoch();
  request.subject = c.partitions[0].id;
  request.left = b.partitions[0].lineage;
  request.right = b.partitions[1].lineage;
  const fpm::MergeOutcome merged = harness.runtime->request_merge(request);
  FPM_EQ(merged.verdict, fpm::DecisionVerdict::Granted);
  harness.advance();

  // The reconciled fabric splits again.
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.split)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment d = harness.runtime->assess();
  FPM_EQ(d.partitions.size(), std::size_t{2});
  for (const fpm::Partition& partition : d.partitions) {
    FPM_CHECK(partition.pending_merge_parents.empty());
    FPM_EQ(partition.parent_lineage, merged.merged_lineage);
    FPM_NE(partition.lineage, genesis);
  }

  // Every superseded lineage is still present in durable lineage, so the
  // history is preserved without restoring any authority.
  const std::vector<fpm::LineageRecord> records = harness.runtime->lineage_records();
  FPM_CHECK(records.size() >= 6u);
  std::vector<fpm::LineageId> seen;
  for (const fpm::LineageRecord& record : records) {
    seen.push_back(record.lineage);
  }
  FPM_CHECK(std::find(seen.begin(), seen.end(), genesis) != seen.end());
  FPM_CHECK(std::find(seen.begin(), seen.end(), merged.merged_lineage) != seen.end());

  // Deterministic lineage: repeating the same shape from the same evidence
  // produces the same identities.
  const fpm::PartitionAssessment repeat = harness.runtime->assess();
  FPM_EQ(repeat.partitions.size(), d.partitions.size());
  for (std::size_t index = 0; index < d.partitions.size(); ++index) {
    FPM_EQ(repeat.partitions[index].lineage, d.partitions[index].lineage);
    FPM_NE(repeat.partitions[index].id, d.partitions[index].id);
  }
}

FPM_TEST(merge, lineage_relation_is_bounded_and_never_assumes_unrelated) {
  fpm::LineageStore store(test_limits());
  const fpm::MembershipDigest first =
      fpm::compute_membership_digest({component(0), component(1)});
  fpm::LineageRecord genesis;
  genesis.sequence = fpm::LineageSequence::from_value(1);
  genesis.lineage = fpm::genesis_lineage_id(first);
  genesis.generation = fpm::PartitionGeneration::from_value(1);
  genesis.membership = first;
  genesis.members = {component(0), component(1)};
  genesis.event = fpm::LineageEventKind::Genesis;
  genesis.epoch = fpm::CoordinatorEpoch::from_value(1);
  genesis.boot = fpm::CoordinatorBootId::generate();
  genesis.decision = fpm::DecisionId::from_validated("d1");
  genesis.digest = fpm::compute_lineage_digest(genesis);
  FPM_CHECK(store.append(genesis));

  const fpm::MembershipDigest second = fpm::compute_membership_digest({component(0)});
  fpm::LineageRecord child = genesis;
  child.sequence = fpm::LineageSequence::from_value(2);
  child.lineage = fpm::split_lineage_id(genesis.lineage, second);
  child.membership = second;
  child.members = {component(0)};
  child.event = fpm::LineageEventKind::Split;
  child.parent_left = genesis.lineage;
  child.digest = fpm::compute_lineage_digest(child);
  FPM_CHECK(store.append(child));

  bool limit = false;
  FPM_EQ(store.relate(genesis.lineage, child.lineage, limit), fpm::LineageRelation::AncestorOf);
  FPM_CHECK(!limit);
  FPM_EQ(store.relate(child.lineage, genesis.lineage, limit), fpm::LineageRelation::DescendantOf);
  FPM_EQ(store.relate(genesis.lineage, genesis.lineage, limit), fpm::LineageRelation::Identical);

  const fpm::MembershipDigest third = fpm::compute_membership_digest({component(5)});
  fpm::LineageRecord unrelated = genesis;
  unrelated.sequence = fpm::LineageSequence::from_value(3);
  unrelated.lineage = fpm::genesis_lineage_id(third);
  unrelated.membership = third;
  unrelated.members = {component(5)};
  unrelated.event = fpm::LineageEventKind::Genesis;
  unrelated.parent_left = fpm::LineageId{};
  unrelated.parent_right = fpm::LineageId{};
  unrelated.digest = fpm::compute_lineage_digest(unrelated);
  FPM_CHECK(store.append(unrelated));
  FPM_EQ(store.relate(genesis.lineage, unrelated.lineage, limit), fpm::LineageRelation::Unrelated);
  FPM_CHECK(!limit);

  // An identity that was never recorded is indeterminate, never "unrelated".
  const fpm::LineageId unknown = fpm::LineageId::from_validated("lffffffffffffffffffffffffffffffff");
  FPM_EQ(store.relate(genesis.lineage, unknown, limit), fpm::LineageRelation::Indeterminate);
}

FPM_TEST(merge, lineage_store_rejects_regression_and_inconsistent_membership) {
  fpm::LineageStore store(test_limits());
  const fpm::MembershipDigest membership =
      fpm::compute_membership_digest({component(0), component(1)});
  fpm::LineageRecord record;
  record.sequence = fpm::LineageSequence::from_value(4);
  record.lineage = fpm::genesis_lineage_id(membership);
  record.generation = fpm::PartitionGeneration::from_value(1);
  record.membership = membership;
  record.members = {component(0), component(1)};
  record.event = fpm::LineageEventKind::Genesis;
  record.epoch = fpm::CoordinatorEpoch::from_value(1);
  record.boot = fpm::CoordinatorBootId::generate();
  record.decision = fpm::DecisionId::from_validated("d1");
  record.digest = fpm::compute_lineage_digest(record);
  FPM_CHECK(store.append(record));
  FPM_CHECK(!store.append(record));

  fpm::LineageRecord regressed = record;
  regressed.sequence = fpm::LineageSequence::from_value(3);
  regressed.generation = fpm::PartitionGeneration::from_value(2);
  regressed.digest = fpm::compute_lineage_digest(regressed);
  FPM_CHECK(!store.append(regressed));

  fpm::LineageRecord inconsistent = record;
  inconsistent.sequence = fpm::LineageSequence::from_value(5);
  inconsistent.generation = fpm::PartitionGeneration::from_value(3);
  inconsistent.members = {component(0)};
  inconsistent.digest = fpm::compute_lineage_digest(inconsistent);
  FPM_CHECK(!store.append(inconsistent));

  fpm::LineageRecord missing_parent = record;
  missing_parent.sequence = fpm::LineageSequence::from_value(6);
  missing_parent.generation = fpm::PartitionGeneration::from_value(4);
  missing_parent.event = fpm::LineageEventKind::Split;
  missing_parent.parent_left = fpm::LineageId::from_validated("lffffffffffffffffffffffffffffffff");
  missing_parent.digest = fpm::compute_lineage_digest(missing_parent);
  FPM_CHECK(!store.append(missing_parent));

  fpm::LineageRecord unsorted = record;
  unsorted.sequence = fpm::LineageSequence::from_value(7);
  unsorted.generation = fpm::PartitionGeneration::from_value(5);
  unsorted.members = {component(1), component(0)};
  unsorted.digest = fpm::compute_lineage_digest(unsorted);
  FPM_CHECK(!store.append(unsorted));
}

FPM_TEST(merge, retirement_is_recorded_and_fences_every_grant) {
  SplitMergeHarness harness;
  FPM_CHECK(harness.runtime->ingest_evidence(harness.timed(harness.whole)).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = harness.runtime->assess();
  const fpm::LineageId lineage = assessment.partitions[0].lineage;
  FPM_CHECK(!assessment.partitions[0].authority.authorized.empty());

  fpm::RetireRequest request;
  request.attempt = attempt(1, "retire");
  request.expected_epoch = harness.runtime->epoch();
  request.lineage = lineage;
  request.requester = fpm::Provenance::from_validated("test");
  const fpm::Decision decision = harness.runtime->retire(request);
  FPM_EQ(decision.verdict, fpm::DecisionVerdict::Granted);
  FPM_CHECK(!decision.fences_issued.empty());
  FPM_EQ(decision.fences_issued.size(), decision.fenced_partitions.size());

  bool retired = false;
  for (const fpm::LineageRecord& record : harness.runtime->lineage_records()) {
    if (record.lineage == lineage && record.event == fpm::LineageEventKind::Retire) {
      retired = true;
    }
  }
  FPM_CHECK(retired);
  FPM_CHECK(!harness.runtime->fences().empty());
}
