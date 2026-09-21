// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// ex_restart_revalidation: a durable store, a restart, interrupted authority,
// and a revalidation that restores it only with fresh evidence.
//
// Lineage is the only part of a partition that survives a restart. Reachability
// evidence, evidence freshness and live authority do not: every authority that
// existed before the restart is reported INTERRUPTED, an ordinary assessment
// does not restore it, and only an explicit revalidation against fresh evidence
// re-derives it under the new epoch. A revalidation with no fresh evidence is
// INDETERMINATE, never a grant.
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "example_support.hpp"

namespace {

namespace fpm = fabric_partition_manager;
using fpm_example::require;

[[nodiscard]] bool has_reason(const std::vector<fpm::Reason>& reasons, fpm::ReasonCode code) {
  for (const fpm::Reason& reason : reasons) {
    if (reason.code == code) {
      return true;
    }
  }
  return false;
}

void body() {
  const fpm::PartitionPolicy policy = fpm_example::count_quorum_policy(1, 3);
  fpm_example::TempWorkspace workspace("ex-restart-revalidation");

  std::uint64_t first_epoch = 0;
  std::size_t first_lineage = 0;
  fpm::LineageId lineage;

  {
    auto runtime = std::make_unique<fpm::PartitionRuntime>(fpm_example::durable_options(policy, workspace.path()));
    (void)fpm_example::setup_synthetic(*runtime, {3});
    const fpm::PartitionAssessment assessment = runtime->assess();
    require(assessment.partitions.size() == 1, "the fixture is one component set");
    require(assessment.decision.verdict == fpm::DecisionVerdict::Granted,
            "the first incarnation must grant authority");
    const fpm::Partition& partition = assessment.partitions.front();
    require(partition.authority.authority_class == fpm::PartitionAuthorityClass::Primary,
            "the first incarnation holds primary authority");
    require(partition.authority.authorized.count() == fpm::authority_capability_count,
            "a primary holds every capability");
    first_epoch = runtime->epoch().value();
    first_lineage = runtime->lineage_records().size();
    lineage = partition.lineage;
    std::cout << "first incarnation epoch=" << first_epoch
              << " partitions=1 lineage=" << lineage.view()
              << " authorized=[" << partition.authority.authorized.render() << "]" << "\n";
    runtime->close();
  }

  // The restart: a new incarnation over the same durable store.
  auto restarted = std::make_unique<fpm::PartitionRuntime>(fpm_example::durable_options(policy, workspace.path()));
  require(restarted->epoch().value() > first_epoch, "a restart must advance the coordinator epoch");
  require(restarted->lineage_records().size() == first_lineage,
          "committed lineage survives a restart");
  require(restarted->status().evidence_bundles == 0,
          "reachability evidence must not survive a restart");
  require(restarted->reachability_summary().fresh_bundle_count == 0,
          "no evidence is fresh before anything is republished");
  require(restarted->interrupted_authorities().size() == 1,
          "the pre-restart authority must be reported interrupted");
  require(has_reason(restarted->restart_reasons(), fpm::ReasonCode::RestartAuthorityInterrupted),
          "the restart must explain that authority was interrupted");
  require(restarted->current_partitions().empty(),
          "no partition holds authority immediately after a restart");
  const std::size_t restart_fences = restarted->fences().size();
  require(restart_fences >= 1, "an interrupted grant is fenced at restart");
  const std::uint64_t restarted_epoch = restarted->epoch().value();
  std::cout << "restart epoch=" << restarted_epoch
            << " lineage-records=" << restarted->lineage_records().size()
            << " fences=" << restart_fences << " evidence=0 interrupted=1" << "\n";

  // Fresh evidence, then an ordinary assessment. The assessment alone does not
  // restore the interrupted authority.
  const fpm::SyntheticFabric fresh = fpm_example::build_fabric(
      {3}, restarted->limits(), 21, true, "restart.publisher", "restart.evidence");
  (void)fpm_example::ingest(*restarted, fresh);
  const fpm::PartitionAssessment observed_again = restarted->assess();
  require(observed_again.partitions.size() == 1, "the republished fabric is one component set");
  const fpm::Partition& candidate = observed_again.partitions.front();
  require(candidate.lineage == lineage, "the re-derived membership keeps its lineage");
  require(candidate.authority.authority_class == fpm::PartitionAuthorityClass::ObserveOnly,
          "an ordinary assessment must not restore interrupted authority");
  require(candidate.authority.basis == fpm::AuthorityBasis::RestartInterrupted,
          "the basis must name the restart interruption");
  require(!candidate.authority.authorized.contains(fpm::AuthorityCapability::ServeWrites),
          "no write capability may be restored by an ordinary assessment");
  require(restarted->interrupted_authorities().size() == 1,
          "the interruption still stands after an ordinary assessment");
  std::cout << "after restart assessment class="
            << fpm::partition_authority_class_name(candidate.authority.authority_class)
            << " basis=" << fpm::authority_basis_name(candidate.authority.basis)
            << " authorized=[" << candidate.authority.authorized.render() << "]"
            << " interrupted=" << restarted->interrupted_authorities().size() << "\n";

  // The explicit revalidation is what restores authority.
  fpm::RevalidationRequest request;
  request.attempt = fpm_example::attempt_token(1);
  request.expected_epoch = restarted->epoch();
  request.requester = fpm::Provenance::from_validated("example");
  const fpm::RevalidationOutcome outcome = restarted->revalidate(request);
  require(outcome.verdict == fpm::DecisionVerdict::Granted,
          "revalidation over fresh evidence must be granted");
  require(outcome.restored.size() == 1, "exactly one authority is restored");
  require(outcome.still_interrupted.empty(), "no authority remains interrupted");
  require(restarted->interrupted_authorities().empty(),
          "a restored authority is no longer interrupted");
  const fpm::Partition* const restored_partition =
      fpm_example::find_by_lineage(restarted->last_assessment().partitions, lineage);
  if (restored_partition == nullptr) {
    fpm_example::fail("the revalidated lineage must be present");
  }
  require(restored_partition->authority.authority_class == fpm::PartitionAuthorityClass::Primary,
          "revalidation must restore primary authority");
  require(restored_partition->authority.authorized.count() == fpm::authority_capability_count,
          "revalidation must restore every capability");
  require(restarted->fences().size() > restart_fences,
          "restoring an interrupted authority fences the pre-restart grant");
  std::cout << "revalidated epoch=" << restarted->epoch().value()
            << " restored=1 class="
            << fpm::partition_authority_class_name(
                   restored_partition->authority.authority_class)
            << " authorized=[" << restored_partition->authority.authorized.render() << "]"
            << " fences=" << restarted->fences().size() << " interrupted=0" << "\n";

  // Without fresh evidence a revalidation is INDETERMINATE. It never converts a
  // durable mark into live authority.
  fpm_example::TempWorkspace cold_workspace("ex-restart-revalidation-cold");
  auto cold = std::make_unique<fpm::PartitionRuntime>(fpm_example::durable_options(policy, cold_workspace.path()));
  const fpm::SyntheticFabric cold_fabric =
      fpm_example::build_fabric({3}, cold->limits(), 31, true, "cold->publisher", "cold->evidence");
  (void)fpm_example::adopt(*cold, cold_fabric);
  fpm::RevalidationRequest cold_request;
  cold_request.attempt = fpm_example::attempt_token(1);
  cold_request.expected_epoch = cold->epoch();
  cold_request.requester = fpm::Provenance::from_validated("example");
  const fpm::RevalidationOutcome cold_outcome = cold->revalidate(cold_request);
  require(cold_outcome.verdict == fpm::DecisionVerdict::Indeterminate,
          "revalidation without fresh evidence must be indeterminate");
  require(cold_outcome.restored.empty(), "nothing may be restored without fresh evidence");
  for (const fpm::Partition& partition : cold->last_assessment().partitions) {
    require(partition.authority.authority_class != fpm::PartitionAuthorityClass::Primary,
            "no primary authority may exist without fresh evidence");
  }
  std::cout << "revalidation without fresh evidence verdict="
            << fpm::decision_verdict_name(cold_outcome.verdict)
            << " restored=0 (an unresolved question is not a grant)" << "\n";
}

}  // namespace

int main() { return fpm_example::run_example("ex_restart_revalidation", &body); }
