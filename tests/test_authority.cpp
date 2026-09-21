// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "fixture.hpp"

using namespace fpm_test;
namespace fpm = fabric_partition_manager;

namespace {

struct Harness {
  explicit Harness(fpm::PartitionPolicy policy)
      : runtime(std::make_unique<fpm::PartitionRuntime>(memory_runtime_options(policy))) {}

  std::unique_ptr<fpm::PartitionRuntime> runtime;
};

fpm::ReachabilityEvidence retimed(const fpm::ReachabilityEvidence& source, std::uint64_t tick,
                                  std::uint64_t validity) {
  fpm::ReachabilityEvidence evidence = source;
  evidence.observed_at_tick = tick;
  evidence.validity_ticks = validity;
  evidence.publisher_boot = fpm::PublisherBootId::generate();
  return evidence;
}

}  // namespace

FPM_TEST(authority, quorum_satisfied_confers_authority) {
  const fpm::SyntheticFabric built = fabric({3});
  Harness harness(count_quorum_policy(1, 3));
  FPM_CHECK(harness.runtime->adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(harness.runtime->ingest_evidence(built.evidence).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = harness.runtime->assess();
  FPM_EQ(assessment.decision.verdict, fpm::DecisionVerdict::Granted);
  FPM_CHECK(assessment.confirmed);
  FPM_EQ(assessment.partitions.size(), std::size_t{1});
  const fpm::Partition& partition = assessment.partitions[0];
  FPM_EQ(partition.authority.authority_class, fpm::PartitionAuthorityClass::Primary);
  FPM_EQ(partition.authority.basis, fpm::AuthorityBasis::QuorumSatisfied);
  FPM_CHECK(partition.authority.confers_authority());
  FPM_EQ(partition.authority.application_state,
         fpm::AuthorityApplicationState::AuthorizationIssued);
  FPM_CHECK(partition.authority.authorized.contains(fpm::AuthorityCapability::ServeWrites));
  FPM_CHECK(partition.authority.matches_class_capabilities(harness.runtime->policy()));
  FPM_EQ(assessment.authoritative_partition, partition.id);
  FPM_CHECK(!partition.authority_sequence.is_zero());
}

FPM_TEST(authority, quorum_not_satisfied_isolates_rather_than_guesses) {
  const fpm::SyntheticFabric built = fabric({2});
  fpm::PartitionPolicy policy = count_quorum_policy(1, 3);
  policy.degraded_authority.min_components = 3;
  Harness harness(policy);
  FPM_CHECK(harness.runtime->adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(harness.runtime->ingest_evidence(built.evidence).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = harness.runtime->assess();
  FPM_EQ(assessment.partitions.size(), std::size_t{1});
  const fpm::Partition& partition = assessment.partitions[0];
  FPM_EQ(partition.authority.authority_class, fpm::PartitionAuthorityClass::Isolated);
  FPM_EQ(partition.authority.basis, fpm::AuthorityBasis::QuorumNotSatisfied);
  FPM_CHECK(partition.authority.authorized.empty());
  FPM_CHECK(partition.authority_sequence.is_zero());
  FPM_EQ(partition.authority.application_state, fpm::AuthorityApplicationState::NotApplicable);
  FPM_EQ(assessment.decision.verdict, fpm::DecisionVerdict::Isolated);
  FPM_CHECK(assessment.authoritative_partition.is_nil());
}

FPM_TEST(authority, reachability_alone_never_confers_authority) {
  // The component set is fully connected and confirmed, and still receives no
  // authority because neither the full nor the degraded quorum is met.
  const fpm::SyntheticFabric built = fabric({4}, 3, 1, 1, true);
  fpm::PartitionPolicy policy = count_quorum_policy(1, 9);
  policy.degraded_authority.min_components = 9;
  Harness harness(policy);
  FPM_CHECK(harness.runtime->adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(harness.runtime->ingest_evidence(built.evidence).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = harness.runtime->assess();
  FPM_CHECK(assessment.confirmed);
  const fpm::Partition& partition = assessment.partitions[0];
  FPM_EQ(partition.member_count(), 4u);
  // Observation says the component set is physically able to act; authority
  // says it may not.
  FPM_CHECK(partition.authority.observed.contains(fpm::AuthorityCapability::CommitDurability));
  FPM_CHECK(partition.authority.authorized.empty());
  FPM_CHECK(!partition.authority.confers_authority());
  FPM_EQ(partition.authority.application_state, fpm::AuthorityApplicationState::NotApplicable);
}

FPM_TEST(authority, partial_decomposition_caps_write_authority) {
  // Two component sets with partial evidence: every crossing pair is unobserved,
  // so the decomposition is a refinement that unknown connectivity could still
  // merge, and write authority is withheld.
  const fpm::SyntheticFabric built = fabric({3, 1}, 3, 1, 1, false);
  fpm::PartitionPolicy policy = count_quorum_policy(1, 3);
  Harness harness(policy);
  FPM_CHECK(harness.runtime->adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(harness.runtime->ingest_evidence(built.evidence).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = harness.runtime->assess();
  FPM_CHECK(!assessment.confirmed);
  FPM_EQ(assessment.partitions.size(), std::size_t{2});
  const fpm::Partition& partition = assessment.partitions[0];
  FPM_EQ(partition.member_count(), 3u);
  FPM_EQ(partition.authority.authority_class, fpm::PartitionAuthorityClass::ReadOnly);
  FPM_CHECK(!partition.authority.authorized.contains(fpm::AuthorityCapability::ServeWrites));
  FPM_CHECK(!partition.authority.authorized.contains(fpm::AuthorityCapability::AdmitMutations));
  FPM_CHECK(partition.authority.authorized.contains(fpm::AuthorityCapability::ServeReads));
  FPM_CHECK(partition.authority.matches_class_capabilities(harness.runtime->policy()));
}

FPM_TEST(authority, strict_majority_is_preferred_over_a_bare_component_count) {
  const fpm::SyntheticFabric built = fabric({2, 2}, 3, 1, 1, true);
  Harness harness(majority_policy(1));
  FPM_CHECK(harness.runtime->adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(harness.runtime->ingest_evidence(built.evidence).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = harness.runtime->assess();
  FPM_EQ(assessment.partitions.size(), std::size_t{2});
  // Neither half holds a strict majority of the four-member authority domain.
  FPM_EQ(assessment.decision.verdict, fpm::DecisionVerdict::Degraded);
  FPM_EQ(count_partitions_with_class(assessment.partitions,
                                     fpm::PartitionAuthorityClass::Primary),
         0u);
}

FPM_TEST(authority, two_independent_quorums_are_a_conflict_and_both_are_reduced) {
  const fpm::SyntheticFabric built = fabric({3, 3}, 3, 1, 1, true);
  Harness harness(count_quorum_policy(1, 2));
  FPM_CHECK(harness.runtime->adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(harness.runtime->ingest_evidence(built.evidence).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = harness.runtime->assess();
  FPM_EQ(assessment.partitions.size(), std::size_t{2});
  FPM_CHECK(assessment.split_brain);
  FPM_EQ(assessment.decision.verdict, fpm::DecisionVerdict::Conflict);
  FPM_EQ(count_partitions_with_class(assessment.partitions,
                                     fpm::PartitionAuthorityClass::Primary),
         0u);
  for (const fpm::Partition& partition : assessment.partitions) {
    FPM_EQ(partition.authority.authority_class, fpm::PartitionAuthorityClass::ObserveOnly);
    FPM_EQ(partition.authority.basis, fpm::AuthorityBasis::SplitBrainConflict);
    FPM_CHECK(partition.authority.authorized.contains(fpm::AuthorityCapability::Observe));
    FPM_CHECK(!partition.authority.authorized.contains(fpm::AuthorityCapability::ServeWrites));
  }
  FPM_CHECK(has_reason_code(assessment.decision.reasons,
                            fpm::ReasonCode::AuthoritySplitBrainConflict));
}

FPM_TEST(authority, policy_may_explicitly_allow_multiple_authoritative_partitions) {
  const fpm::SyntheticFabric built = fabric({3, 3}, 3, 1, 1, true);
  fpm::PartitionPolicy policy = count_quorum_policy(1, 2);
  policy.allow_multiple_authoritative_partitions = true;
  Harness harness(policy);
  FPM_CHECK(harness.runtime->adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(harness.runtime->ingest_evidence(built.evidence).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = harness.runtime->assess();
  FPM_CHECK(!assessment.split_brain);
  FPM_EQ(count_partitions_with_class(assessment.partitions,
                                     fpm::PartitionAuthorityClass::Primary),
         2u);
}

FPM_TEST(authority, explicit_isolation_overrides_a_satisfied_quorum) {
  const fpm::SyntheticFabric built = fabric({3});
  Harness harness(count_quorum_policy(1, 3));
  FPM_CHECK(harness.runtime->adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(harness.runtime->ingest_evidence(built.evidence).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment before = harness.runtime->assess();
  FPM_EQ(before.partitions[0].authority.authority_class, fpm::PartitionAuthorityClass::Primary);
  const fpm::LineageId lineage = before.partitions[0].lineage;

  fpm::IsolationRequest request;
  request.attempt = attempt(1, "isolate");
  request.expected_epoch = harness.runtime->epoch();
  request.lineage = lineage;
  request.cause = fpm::ReasonCode::IsolationApplied;
  request.requester = fpm::Provenance::from_validated("test");
  const fpm::IsolationOutcome isolated = harness.runtime->isolate(request);
  FPM_EQ(isolated.verdict, fpm::DecisionVerdict::Isolated);

  const fpm::PartitionAssessment during = harness.runtime->last_assessment();
  FPM_EQ(during.partitions[0].authority.authority_class, fpm::PartitionAuthorityClass::Isolated);
  FPM_EQ(during.partitions[0].authority.basis, fpm::AuthorityBasis::ExplicitIsolation);
  FPM_CHECK(during.partitions[0].authority.authorized.empty());

  fpm::IsolationRequest clearing = request;
  clearing.attempt = attempt(2, "clear");
  const fpm::IsolationOutcome cleared = harness.runtime->clear_isolation(clearing);
  FPM_EQ(cleared.verdict, fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment after = harness.runtime->last_assessment();
  FPM_EQ(after.partitions[0].authority.authority_class, fpm::PartitionAuthorityClass::Primary);
}

FPM_TEST(authority, isolation_of_an_unknown_lineage_is_refused) {
  const fpm::SyntheticFabric built = fabric({3});
  Harness harness(count_quorum_policy(1, 3));
  FPM_CHECK(harness.runtime->adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  fpm::IsolationRequest request;
  request.attempt = attempt(1, "isolate-unknown");
  request.expected_epoch = harness.runtime->epoch();
  request.lineage = fpm::LineageId::from_validated("lffffffffffffffffffffffffffffffff");
  const fpm::IsolationOutcome outcome = harness.runtime->isolate(request);
  FPM_EQ(outcome.verdict, fpm::DecisionVerdict::Denied);
  FPM_CHECK(has_reason_code(outcome.decision.reasons,
                            fpm::ReasonCode::IsolationRejectedUnknownSubject));
}

FPM_TEST(authority, epoch_binding_is_enforced_on_every_mutation) {
  const fpm::SyntheticFabric built = fabric({3});
  Harness harness(count_quorum_policy(1, 3));
  FPM_CHECK(harness.runtime->adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  fpm::IsolationRequest request;
  request.attempt = attempt(1, "epoch");
  request.expected_epoch = fpm::CoordinatorEpoch::from_value(
      harness.runtime->epoch().value() + 99);
  request.lineage = fpm::LineageId::from_validated("l00000000000000000000000000000000");
  const fpm::IsolationOutcome outcome = harness.runtime->isolate(request);
  FPM_EQ(outcome.verdict, fpm::DecisionVerdict::Denied);
  FPM_CHECK(has_reason_code(outcome.decision.reasons, fpm::ReasonCode::EpochMismatchReason));
}

FPM_TEST(authority, attempt_identity_is_fenced_against_replay_and_regression) {
  const fpm::SyntheticFabric built = fabric({3});
  Harness harness(count_quorum_policy(1, 3));
  FPM_CHECK(harness.runtime->adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(harness.runtime->ingest_evidence(built.evidence).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = harness.runtime->assess();
  const fpm::LineageId lineage = assessment.partitions[0].lineage;

  fpm::IsolationRequest request;
  request.attempt = attempt(5, "once");
  request.expected_epoch = harness.runtime->epoch();
  request.lineage = lineage;
  FPM_EQ(harness.runtime->isolate(request).verdict, fpm::DecisionVerdict::Isolated);

  const fpm::IsolationOutcome replay = harness.runtime->clear_isolation(request);
  FPM_EQ(replay.verdict, fpm::DecisionVerdict::Invalid);
  FPM_CHECK(has_reason_code(replay.decision.reasons, fpm::ReasonCode::AttemptDuplicate));

  fpm::IsolationRequest regressed = request;
  regressed.attempt = attempt(2, "regressed");
  const fpm::IsolationOutcome outcome = harness.runtime->clear_isolation(regressed);
  FPM_EQ(outcome.verdict, fpm::DecisionVerdict::Invalid);
  FPM_CHECK(has_reason_code(outcome.decision.reasons, fpm::ReasonCode::AttemptRegression));
}

FPM_TEST(authority, acknowledgement_is_not_a_verified_effect) {
  const fpm::SyntheticFabric built = fabric({3});
  Harness harness(count_quorum_policy(1, 3));
  FPM_CHECK(harness.runtime->adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(harness.runtime->ingest_evidence(built.evidence).verdict ==
            fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = harness.runtime->assess();
  const fpm::PartitionId partition = assessment.partitions[0].id;
  FPM_EQ(harness.runtime->application_state(partition),
         fpm::AuthorityApplicationState::AuthorizationIssued);

  fpm::AuthorityAcknowledgement acknowledgement;
  acknowledgement.attempt = fpm::AttemptId::from_validated("ack-1");
  acknowledgement.partition = partition;
  acknowledgement.accepted = true;
  acknowledgement.applied = fpm::CapabilitySet::all();
  FPM_CHECK(harness.runtime->record_acknowledgement(acknowledgement).verdict ==
            fpm::DecisionVerdict::Observed);
  FPM_EQ(harness.runtime->application_state(partition),
         fpm::AuthorityApplicationState::AcknowledgedBySubject);

  // An effect cannot be verified before the subject acknowledged, and cannot be
  // verified for an attempt that was never acknowledged.
  fpm::VerifiedEffect premature;
  premature.partition = partition;
  premature.attempt = fpm::AttemptId::from_validated("ack-unknown");
  premature.confirmed = fpm::CapabilitySet::of(fpm::AuthorityCapability::Observe);
  premature.evidence = fpm::EvidenceId::from_validated("evidence");
  const fpm::Decision refused = harness.runtime->record_verified_effect(premature);
  FPM_EQ(refused.verdict, fpm::DecisionVerdict::Invalid);
  FPM_CHECK(has_reason_code(refused.reasons, fpm::ReasonCode::AttemptUnknown));

  fpm::VerifiedEffect effect;
  effect.partition = partition;
  effect.attempt = acknowledgement.attempt;
  effect.confirmed = fpm::CapabilitySet::of(fpm::AuthorityCapability::ServeReads);
  effect.evidence = fpm::EvidenceId::from_validated("evidence");
  FPM_CHECK(harness.runtime->record_verified_effect(effect).verdict ==
            fpm::DecisionVerdict::Observed);
  FPM_EQ(harness.runtime->application_state(partition),
         fpm::AuthorityApplicationState::VerifiedByObservation);

  // An acknowledgement that claims more than was authorized is refused.
  fpm::AuthorityAcknowledgement excessive;
  excessive.attempt = fpm::AttemptId::from_validated("ack-2");
  excessive.partition = partition;
  excessive.accepted = true;
  excessive.applied = fpm::CapabilitySet::all();
  FPM_CHECK(harness.runtime->record_acknowledgement(excessive).verdict ==
            fpm::DecisionVerdict::Observed);
  fpm::Partition isolated_partition = assessment.partitions[0];
  isolated_partition.authority.authorized = fpm::CapabilitySet::none();
  fpm::AuthorityAcknowledgement on_unauthorized;
  on_unauthorized.attempt = fpm::AttemptId::from_validated("ack-3");
  on_unauthorized.partition = fpm::PartitionId::from_validated("p0000000000000000000000000000000f");
  on_unauthorized.accepted = true;
  on_unauthorized.applied = fpm::CapabilitySet::none();
  FPM_CHECK(harness.runtime->record_acknowledgement(on_unauthorized).verdict ==
            fpm::DecisionVerdict::Invalid);
}

FPM_TEST(authority, authority_vectors_keep_every_stage_separate) {
  const fpm::PartitionPolicy policy = count_quorum_policy(1, 3);
  const std::vector<fpm::Reason> reasons;
  const fpm::AuthorityVector isolated = fpm::isolated_authority(
      fpm::AuthorityBasis::QuorumNotSatisfied, fpm::CapabilitySet::all(), reasons);
  FPM_CHECK(isolated.observed.contains(fpm::AuthorityCapability::ServeWrites));
  FPM_CHECK(isolated.authorized.empty());
  FPM_CHECK(isolated.eligible.empty());
  FPM_CHECK(!isolated.confers_authority());
  FPM_CHECK(isolated.matches_class_capabilities(policy));

  const fpm::AuthorityVector observed = fpm::observed_only_authority(
      fpm::CapabilitySet::of(fpm::AuthorityCapability::ServeReads), reasons);
  FPM_EQ(observed.authority_class, fpm::PartitionAuthorityClass::Unassigned);
  FPM_EQ(observed.application_state, fpm::AuthorityApplicationState::NotApplicable);
  FPM_CHECK(observed.authorized.empty());
  FPM_CHECK(!observed.revocable);

  const fpm::CapabilitySet eligible =
      fpm::CapabilitySet::all();
  const fpm::CapabilitySet authorized = fpm::CapabilitySet::of(fpm::AuthorityCapability::Observe);
  const fpm::AuthorityVector granted = fpm::authorized_authority(
      fpm::PartitionAuthorityClass::ObserveOnly, fpm::AuthorityBasis::UnknownEvidence,
      fpm::CapabilitySet::all(), eligible, authorized, reasons);
  FPM_EQ(granted.authority_class, fpm::PartitionAuthorityClass::ObserveOnly);
  FPM_EQ(granted.withheld, eligible.without(authorized));
  FPM_CHECK(granted.recommended == authorized);
  FPM_CHECK(granted.matches_class_capabilities(policy));

  FPM_CHECK(fpm::capabilities_for_class(policy, fpm::PartitionAuthorityClass::Isolated).empty());
  FPM_EQ(fpm::capabilities_for_class(policy, fpm::PartitionAuthorityClass::Primary).bits(),
         fpm::CapabilitySet::all().bits());
}
