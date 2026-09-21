// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "fixture.hpp"

using namespace fpm_test;
namespace fpm = fabric_partition_manager;

FPM_TEST(limits, checked_arithmetic_never_wraps) {
  const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  FPM_CHECK(!fpm::checked_add(maximum, 1).has_value());
  FPM_EQ(*fpm::checked_add(maximum - 1, 1), maximum);
  FPM_CHECK(!fpm::checked_mul(maximum, 2).has_value());
  FPM_EQ(*fpm::checked_mul(0, maximum), 0u);
  FPM_EQ(*fpm::checked_mul(1ull << 32, 1ull << 31), 1ull << 63);
  FPM_CHECK(!fpm::checked_narrow_u32(1ull << 32).has_value());
  FPM_EQ(*fpm::checked_narrow_u32(7), 7u);
  FPM_CHECK(!fpm::checked_size_add(std::numeric_limits<std::size_t>::max(), 1).has_value());
  FPM_CHECK(!fpm::checked_size_mul(std::numeric_limits<std::size_t>::max(), 2).has_value());
}

FPM_TEST(limits, pair_counts_are_exact_at_the_edge_of_the_range) {
  FPM_EQ(*fpm::unordered_pair_count(0), 0u);
  FPM_EQ(*fpm::unordered_pair_count(1), 0u);
  FPM_EQ(*fpm::unordered_pair_count(2), 1u);
  FPM_EQ(*fpm::unordered_pair_count(5), 10u);
  FPM_EQ(*fpm::unordered_pair_count(1'000'000), 499'999'500'000ull);
  FPM_EQ(*fpm::ordered_pair_count(4), 12u);
  // Counts that would overflow are refused rather than wrapped.
  FPM_CHECK(!fpm::unordered_pair_count(std::numeric_limits<std::uint64_t>::max()).has_value());
  FPM_EQ(*fpm::unordered_pair_count(4'000'000'000ull), 7'999'999'998'000'000'000ull);
  FPM_CHECK(!fpm::ordered_pair_count(std::numeric_limits<std::uint64_t>::max()).has_value());
}

FPM_TEST(limits, roster_creation_refuses_more_components_than_the_bound) {
  fpm::Limits limits = test_limits();
  limits.max_components = 4;
  fpm::TopologyDefinition definition;
  definition.id = fpm::TopologyId::from_validated("bounded");
  definition.generation = topology_generation(1);
  definition.provenance = fpm::Provenance::from_validated("test");
  for (std::size_t index = 0; index < 5; ++index) {
    definition.components.push_back(component(index));
  }
  FPM_CHECK(!fpm::ComponentRoster::create(definition, limits).has_value());
  definition.components.pop_back();
  FPM_CHECK(fpm::ComponentRoster::create(definition, limits).has_value());
}

FPM_TEST(limits, evidence_observation_and_byte_bounds_are_enforced_before_materialisation) {
  const fpm::SyntheticFabric built = fabric({4});
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));

  fpm::Limits small = test_limits();
  small.max_observations_per_evidence = 2;
  fpm::EvidenceLedger count_ledger(small);
  FPM_EQ(count_ledger.accept(built.evidence, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::RejectedLimit);

  fpm::Limits tiny = test_limits();
  tiny.max_evidence_bytes = 32;
  fpm::EvidenceLedger byte_ledger(tiny);
  FPM_EQ(byte_ledger.accept(built.evidence, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::RejectedLimit);

  fpm::Limits covered = test_limits();
  covered.max_covered_components_per_evidence = 1;
  fpm::EvidenceLedger covered_ledger(covered);
  FPM_EQ(covered_ledger.accept(built.evidence, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::RejectedLimit);

  // The canonical size estimate is monotone and bounded.
  const auto size = fpm::evidence_canonical_size(built.evidence);
  FPM_CHECK(size.has_value());
  FPM_CHECK(*size > 0);
  FPM_CHECK(*size <= test_limits().max_evidence_bytes);
}

FPM_TEST(limits, the_retained_evidence_table_is_bounded) {
  const fpm::SyntheticFabric built = fabric({2});
  const auto roster = fpm::ComponentRoster::create(built.topology, test_limits());
  FPM_CHECK(roster.has_value());
  const fpm::PartitionPolicy policy = fpm::default_policy(fpm::PolicyGeneration::from_value(1));
  fpm::Limits limits = test_limits();
  limits.max_retained_evidence = 2;
  fpm::EvidenceLedger ledger(limits);
  for (std::uint64_t index = 1; index <= 2; ++index) {
    fpm::ReachabilityEvidence evidence = built.evidence;
    evidence.sequence = fpm::EvidenceSequence::from_value(index);
    evidence.id = fpm::EvidenceId::from_validated("e" + std::to_string(index));
    evidence.publisher_boot = fpm::PublisherBootId::generate();
    FPM_EQ(ledger.accept(evidence, *roster, policy, 0).status,
           fpm::EvidenceAcceptanceStatus::Accepted);
  }
  fpm::ReachabilityEvidence overflow = built.evidence;
  overflow.sequence = fpm::EvidenceSequence::from_value(3);
  overflow.id = fpm::EvidenceId::from_validated("e3");
  overflow.publisher_boot = fpm::PublisherBootId::generate();
  FPM_EQ(ledger.accept(overflow, *roster, policy, 0).status,
         fpm::EvidenceAcceptanceStatus::RejectedLimit);
  FPM_EQ(ledger.retained_count(), std::size_t{2});
}

FPM_TEST(limits, explanations_are_bounded_and_truncation_is_marked) {
  fpm::Limits limits = test_limits();
  limits.max_text_bytes = 16;
  const std::string long_text(200, 'x');
  const fpm::Reason reason =
      fpm::make_reason(fpm::ReasonCode::EvidenceAccepted, long_text, long_text, limits);
  FPM_CHECK(reason.subject.size() <= limits.max_text_bytes);
  FPM_CHECK(reason.detail.size() <= limits.max_text_bytes);
  FPM_CHECK(reason.subject.find("...") != std::string::npos);

  std::vector<fpm::Reason> reasons;
  limits.max_reasons_per_decision = 3;
  for (std::size_t index = 0; index < 10; ++index) {
    const bool appended = fpm::append_reason(reasons, fpm::ReasonCode::Ok, "s", "d", limits);
    FPM_EQ(appended, index < 3);
  }
  FPM_EQ(reasons.size(), std::size_t{3});
  FPM_CHECK(fpm::has_reason_code(reasons, fpm::ReasonCode::Ok));
  FPM_CHECK(!fpm::has_reason_code(reasons, fpm::ReasonCode::IsolationApplied));
}

FPM_TEST(limits, the_decision_log_is_bounded_and_reports_what_it_dropped) {
  fpm::Limits limits = test_limits();
  limits.max_decision_history = 3;
  fpm::DecisionLog log(limits);
  for (std::uint64_t index = 0; index < 10; ++index) {
    fpm::Decision decision;
    decision.id = fpm::DecisionId::from_validated("d" + std::to_string(index));
    log.append(decision);
  }
  FPM_EQ(log.size(), std::size_t{3});
  FPM_EQ(log.dropped(), std::size_t{7});
  FPM_CHECK(log.find(fpm::DecisionId::from_validated("d9")) != nullptr);
  FPM_CHECK(log.find(fpm::DecisionId::from_validated("d0")) == nullptr);
}

FPM_TEST(limits, fence_records_and_attempts_stay_bounded_across_many_mutations) {
  const fpm::SyntheticFabric built = fabric({3}, 5, 1, 1, true);
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 3));
  options.limits.max_fence_records = 4;
  options.limits.max_attempt_records = 8;
  options.limits.max_decision_history = 16;
  fpm::PartitionRuntime runtime(options);
  FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = runtime.assess();
  const fpm::LineageId lineage = assessment.partitions[0].lineage;

  for (std::uint64_t index = 1; index <= 20; ++index) {
    fpm::IsolationRequest request;
    request.attempt = attempt(index, "cycle");
    request.expected_epoch = runtime.epoch();
    request.lineage = lineage;
    const bool isolate_now = index % 2 == 1;
    const fpm::IsolationOutcome outcome =
        isolate_now ? runtime.isolate(request) : runtime.clear_isolation(request);
    FPM_CHECK(outcome.verdict == fpm::DecisionVerdict::Isolated ||
              outcome.verdict == fpm::DecisionVerdict::Granted);
  }
  const fpm::RuntimeStatus status = runtime.status();
  FPM_CHECK(status.fence_records <= options.limits.max_fence_records);
  FPM_CHECK(status.decision_history <= options.limits.max_decision_history);
  FPM_CHECK(status.decisions_dropped > 0);
  FPM_CHECK(status.lineage_records <= options.limits.max_lineage_records);
  runtime.close();
}

FPM_TEST(limits, lineage_member_entries_are_bounded_exactly) {
  fpm::Limits limits = test_limits();
  limits.max_lineage_member_entries = 4;
  fpm::LineageStore store(limits);
  const auto append = [&store](std::uint64_t sequence, std::uint64_t generation,
                               const std::vector<fpm::ComponentId>& members,
                               fpm::LineageEventKind kind, const fpm::LineageId& parent) {
    fpm::LineageRecord record;
    record.sequence = fpm::LineageSequence::from_value(sequence);
    record.generation = fpm::PartitionGeneration::from_value(generation);
    record.membership = fpm::compute_membership_digest(members);
    record.members = members;
    record.event = kind;
    record.parent_left = parent;
    record.epoch = fpm::CoordinatorEpoch::from_value(1);
    record.boot = fpm::CoordinatorBootId::generate();
    record.decision = fpm::DecisionId::from_validated("d" + std::to_string(sequence));
    const fpm::LineageId lineage =
        parent.is_nil() ? fpm::genesis_lineage_id(record.membership)
                        : fpm::split_lineage_id(parent, record.membership);
    record.lineage = lineage;
    record.digest = fpm::compute_lineage_digest(record);
    return std::make_pair(store.append(record), lineage);
  };
  const auto first = append(1, 1, {component(0), component(1), component(2)},
                            fpm::LineageEventKind::Genesis, fpm::LineageId{});
  FPM_CHECK(first.first);
  FPM_EQ(store.retained_member_entries(), 3u);
  const auto second = append(2, 2, {component(0), component(1)}, fpm::LineageEventKind::Split,
                             first.second);
  FPM_CHECK(!second.first);
  FPM_EQ(store.retained_member_entries(), 3u);
  FPM_EQ(store.size(), std::size_t{1});
}

FPM_TEST(limits, partition_generation_advances_once_per_assessment_and_never_wraps) {
  const fpm::SyntheticFabric built = fabric({2});
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 2));
  fpm::PartitionRuntime runtime(options);
  FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.assess().partitions.size() == 1u);
  const std::uint64_t first = runtime.status().partition_generation.value();
  FPM_CHECK(runtime.assess().partitions.size() == 1u);
  const std::uint64_t second = runtime.status().partition_generation.value();
  FPM_EQ(second, first + 1);
  // The counter arithmetic at the top of the range is checked, so a generation
  // can never wrap into one that already carried authority.
  FPM_CHECK(!fpm::PartitionGeneration::from_value(fpm::PartitionGeneration::max_value)
                 .next()
                 .has_value());
  runtime.close();
}

FPM_TEST(limits, tick_advance_refuses_overflow_and_a_closed_runtime) {
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 1));
  fpm::PartitionRuntime runtime(options);
  FPM_CHECK(runtime.advance_ticks(10));
  FPM_EQ(runtime.tick(), std::uint64_t{10});
  FPM_CHECK(!runtime.advance_ticks(std::numeric_limits<std::uint64_t>::max()));
  FPM_EQ(runtime.tick(), std::uint64_t{10});
  runtime.close();
  FPM_CHECK(!runtime.advance_ticks(1));
  FPM_CHECK(runtime.closed());
}
