// Fabric Partition Manager 1.0.0 - Summon Software Labs
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "fixture.hpp"

using namespace fpm_test;
namespace fpm = fabric_partition_manager;

FPM_TEST(lifecycle, a_decision_binds_every_authority_bearing_dependency) {
  const fpm::SyntheticFabric built = fabric({3});
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 3));
  fpm::PartitionRuntime runtime(options);
  FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = runtime.assess();
  const fpm::DecisionBindings& bindings = assessment.decision.bindings;
  FPM_EQ(bindings.epoch, runtime.epoch());
  FPM_EQ(bindings.boot, runtime.boot());
  FPM_EQ(bindings.incarnation, runtime.incarnation());
  FPM_EQ(bindings.topology_generation, topology_generation(1));
  FPM_CHECK(!bindings.topology_digest.is_zero());
  FPM_EQ(bindings.policy_generation, fpm::PolicyGeneration::from_value(1));
  FPM_EQ(bindings.partition_generation, assessment.generation);
  FPM_CHECK(!bindings.decision_sequence.is_zero());
  FPM_EQ(bindings.evaluated_at_tick, runtime.tick());
  FPM_EQ(assessment.decision.scope.kind, fpm::ScopeKind::Fabric);
  FPM_EQ(assessment.decision.subjects.size(), assessment.partitions.size());
  FPM_CHECK(assessment.decision.revocable);
  FPM_CHECK(!assessment.decision.revocation_triggers.empty());
  FPM_CHECK(!assessment.decision.render().empty());
}

FPM_TEST(lifecycle, adoption_is_idempotent_and_rejects_conflicts_and_regressions) {
  const fpm::SyntheticFabric built = fabric({2});
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 2));
  fpm::PartitionRuntime runtime(options);
  FPM_EQ(runtime.adopt_topology(built.topology).verdict, fpm::DecisionVerdict::Granted);
  FPM_EQ(runtime.adopt_topology(built.topology).verdict, fpm::DecisionVerdict::Observed);

  fpm::TopologyDefinition conflicting = built.topology;
  conflicting.components.pop_back();
  FPM_EQ(runtime.adopt_topology(conflicting).verdict, fpm::DecisionVerdict::Conflict);

  fpm::TopologyDefinition regressed = built.topology;
  regressed.generation = topology_generation(0);
  FPM_EQ(runtime.adopt_topology(regressed).verdict, fpm::DecisionVerdict::Invalid);

  FPM_EQ(runtime.adopt_topology(fpm::TopologyDefinition{}).verdict,
         fpm::DecisionVerdict::Invalid);

  fpm::TopologyDefinition next = built.topology;
  next.generation = topology_generation(2);
  FPM_EQ(runtime.adopt_topology(next).verdict, fpm::DecisionVerdict::Granted);
  FPM_EQ(runtime.roster().generation(), topology_generation(2));
  FPM_CHECK(runtime.adopted_topology().has_value());
}

FPM_TEST(lifecycle, evidence_cannot_be_ingested_before_a_roster_exists) {
  const fpm::SyntheticFabric built = fabric({2});
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 2));
  fpm::PartitionRuntime runtime(options);
  const fpm::Decision decision = runtime.ingest_evidence(built.evidence);
  FPM_EQ(decision.verdict, fpm::DecisionVerdict::Invalid);
  FPM_CHECK(has_reason_code(decision.reasons, fpm::ReasonCode::SubjectUnknown));
  const fpm::PartitionAssessment assessment = runtime.assess();
  FPM_EQ(assessment.decision.verdict, fpm::DecisionVerdict::Invalid);
  FPM_CHECK(assessment.partitions.empty());
}

FPM_TEST(lifecycle, a_policy_change_fences_every_live_authority) {
  const fpm::SyntheticFabric built = fabric({3});
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 3));
  fpm::PartitionRuntime runtime(options);
  FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment before = runtime.assess();
  FPM_CHECK(!before.partitions[0].authority.authorized.empty());
  FPM_CHECK(!before.partitions[0].authority_sequence.is_zero());

  fpm::PartitionPolicy next = runtime.policy();
  next.generation = fpm::PolicyGeneration::from_value(2);
  next.id = fpm::PolicyId::from_validated("test.next");
  const fpm::Decision decision = runtime.set_policy(next);
  FPM_EQ(decision.verdict, fpm::DecisionVerdict::Granted);
  FPM_CHECK(!runtime.fences().empty());

  fpm::PartitionPolicy stale = next;
  stale.generation = fpm::PolicyGeneration::from_value(1);
  FPM_EQ(runtime.set_policy(stale).verdict, fpm::DecisionVerdict::Invalid);

  FPM_EQ(runtime.set_policy(fpm::PartitionPolicy{}).verdict, fpm::DecisionVerdict::Invalid);
}

FPM_TEST(lifecycle, a_closed_runtime_refuses_every_mutation_deterministically) {
  const fpm::SyntheticFabric built = fabric({3});
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 3));
  fpm::PartitionRuntime runtime(options);
  FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(!runtime.assess().partitions.empty());
  runtime.close();
  FPM_CHECK(runtime.closed());
  runtime.close();

  FPM_EQ(runtime.adopt_topology(built.topology).verdict, fpm::DecisionVerdict::Invalid);
  FPM_EQ(runtime.ingest_evidence(built.evidence).verdict, fpm::DecisionVerdict::Invalid);
  FPM_EQ(runtime.assess().decision.verdict, fpm::DecisionVerdict::Invalid);
  FPM_CHECK(runtime.policy().canonicalised(test_limits()).has_value());

  fpm::IsolationRequest isolation;
  isolation.attempt = attempt(1, "closed");
  isolation.expected_epoch = runtime.epoch();
  isolation.lineage = fpm::LineageId::from_validated("l00000000000000000000000000000000");
  FPM_EQ(runtime.isolate(isolation).verdict, fpm::DecisionVerdict::Invalid);
  fpm::RetireRequest retire;
  retire.attempt = attempt(2, "closed");
  retire.expected_epoch = runtime.epoch();
  retire.lineage = isolation.lineage;
  FPM_EQ(runtime.retire(retire).verdict, fpm::DecisionVerdict::Invalid);
  fpm::MergeRequest merge;
  merge.attempt = attempt(3, "closed");
  merge.expected_epoch = runtime.epoch();
  merge.subject = fpm::PartitionId::from_validated("p00000000000000000000000000000000");
  merge.left = isolation.lineage;
  merge.right = fpm::LineageId::from_validated("l11111111111111111111111111111111");
  FPM_EQ(runtime.request_merge(merge).verdict, fpm::DecisionVerdict::Invalid);
  fpm::RevalidationRequest revalidation;
  revalidation.attempt = attempt(4, "closed");
  revalidation.expected_epoch = runtime.epoch();
  FPM_EQ(runtime.revalidate(revalidation).verdict, fpm::DecisionVerdict::Invalid);
}

FPM_TEST(lifecycle, the_decision_history_records_the_full_sequence) {
  const fpm::SyntheticFabric built = fabric({3}, 5, 1, 1, true);
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 3));
  fpm::PartitionRuntime runtime(options);
  FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = runtime.assess();
  const fpm::LineageId lineage = assessment.partitions[0].lineage;

  fpm::IsolationRequest isolation;
  isolation.attempt = attempt(1, "hist");
  isolation.expected_epoch = runtime.epoch();
  isolation.lineage = lineage;
  (void)runtime.isolate(isolation);

  fpm::RetireRequest retire;
  retire.attempt = attempt(2, "hist");
  retire.expected_epoch = runtime.epoch();
  retire.lineage = lineage;
  (void)runtime.retire(retire);

  const std::vector<fpm::Decision> history = runtime.decision_history();
  FPM_CHECK(history.size() >= 6u);
  std::vector<fpm::DecisionKind> kinds;
  for (const fpm::Decision& decision : history) {
    kinds.push_back(decision.kind);
    FPM_CHECK(!decision.id.is_nil());
    FPM_CHECK(!decision.render().empty());
  }
  FPM_CHECK(std::find(kinds.begin(), kinds.end(), fpm::DecisionKind::AdoptTopology) != kinds.end());
  FPM_CHECK(std::find(kinds.begin(), kinds.end(), fpm::DecisionKind::IngestEvidence) !=
            kinds.end());
  FPM_CHECK(std::find(kinds.begin(), kinds.end(), fpm::DecisionKind::AssessFabric) != kinds.end());
  FPM_CHECK(std::find(kinds.begin(), kinds.end(), fpm::DecisionKind::IsolateComponents) !=
            kinds.end());
  FPM_CHECK(std::find(kinds.begin(), kinds.end(), fpm::DecisionKind::Retire) != kinds.end());
  FPM_CHECK(runtime.find_decision(assessment.decision.id).has_value());
  FPM_CHECK(!runtime.find_decision(fpm::DecisionId::from_validated("d999999")).has_value());

  // Decision sequences advance strictly.
  for (std::size_t index = 1; index < history.size(); ++index) {
    FPM_CHECK(history[index].bindings.decision_sequence.value() >
              history[index - 1].bindings.decision_sequence.value());
  }
}

FPM_TEST(lifecycle, partition_queries_agree_with_the_last_assessment) {
  const fpm::SyntheticFabric built = fabric({2, 3});
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 4));
  fpm::PartitionRuntime runtime(options);
  FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
  const fpm::PartitionAssessment assessment = runtime.assess();
  const std::vector<fpm::Partition>& current = runtime.current_partitions();
  FPM_EQ(current.size(), assessment.partitions.size());
  for (const fpm::Partition& partition : assessment.partitions) {
    const auto found = runtime.find_partition(partition.id);
    FPM_CHECK(found.has_value());
    FPM_EQ(found->membership, partition.membership);
    FPM_EQ(found->lineage, partition.lineage);
  }
  FPM_CHECK(!runtime.find_partition(
                 fpm::PartitionId::from_validated("p00000000000000000000000000000000"))
                 .has_value());
  const fpm::ReachabilitySummary summary = runtime.reachability_summary();
  FPM_CHECK(summary.valid);
  FPM_CHECK(summary.confirmed);
  FPM_EQ(summary.component_count, std::size_t{2});
  FPM_EQ(summary.counters.undirected_reachable, summary.undirected_reachable());
  FPM_EQ(runtime.last_assessment().decision.id, assessment.decision.id);
}

FPM_TEST(lifecycle, an_out_of_scope_lineage_is_surfaced_not_silently_ignored) {
  const fpm::SyntheticFabric built = fabric({3});
  fpm::RuntimeOptions options = memory_runtime_options(count_quorum_policy(1, 3));
  fpm::PartitionRuntime runtime(options);
  FPM_CHECK(runtime.adopt_topology(built.topology).verdict == fpm::DecisionVerdict::Granted);
  FPM_CHECK(runtime.ingest_evidence(built.evidence).verdict == fpm::DecisionVerdict::Granted);
  (void)runtime.assess();
  fpm::RetireRequest retire;
  retire.attempt = attempt(1, "scope");
  retire.expected_epoch = runtime.epoch();
  retire.lineage = fpm::LineageId::from_validated("lffffffffffffffffffffffffffffffff");
  const fpm::Decision decision = runtime.retire(retire);
  FPM_EQ(decision.verdict, fpm::DecisionVerdict::Denied);
  FPM_CHECK(has_reason_code(decision.reasons, fpm::ReasonCode::SubjectUnknown));
}

FPM_TEST(lifecycle, every_scope_kind_renders_deterministically) {
  FPM_EQ(fpm::fabric_scope().kind, fpm::ScopeKind::Fabric);
  const fpm::Scope partition =
      fpm::partition_scope(fpm::PartitionId::from_validated("p0000000000000000000000000000000a"));
  FPM_EQ(partition.kind, fpm::ScopeKind::Partition);
  FPM_CHECK(partition.id.view().substr(0, 10) == std::string_view("partition:"));
  const fpm::Scope component_scope = fpm::component_scope(component(3));
  FPM_EQ(component_scope.kind, fpm::ScopeKind::Component);
  FPM_CHECK(component_scope.id.view().substr(0, 10) == std::string_view("component:"));
  FPM_EQ(fpm::named_scope(fpm::ScopeKind::Domain, "d").id.view(), std::string_view("d"));
  FPM_EQ(fpm::scope_kind_name(fpm::ScopeKind::Component), std::string_view("COMPONENT"));
}
