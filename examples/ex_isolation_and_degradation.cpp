// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// ex_isolation_and_degradation: isolate a lineage, observe ISOLATED, clear the
// directive and observe recovery.
//
// Isolation is bound to lineage, not to a partition identity, so it survives a
// re-derivation of the same membership under a later partition generation. An
// isolated subject holds nothing at all: every capability it could physically
// exercise is denied rather than withheld.
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "example_support.hpp"

namespace {

namespace fpm = fabric_partition_manager;
using fpm_example::require;

void body() {
  const fpm::PartitionPolicy policy = fpm_example::count_quorum_policy(1, 3);
  fpm::PartitionRuntime runtime(fpm_example::memory_options(policy));
  (void)fpm_example::setup_synthetic(runtime, {3});

  const fpm::PartitionAssessment initial = runtime.assess();
  require(initial.partitions.size() == 1, "the fixture is one component set");
  require(initial.decision.verdict == fpm::DecisionVerdict::Granted,
          "the fabric must be granted authority before it is isolated");
  const fpm::LineageId lineage = initial.partitions.front().lineage;
  require(initial.partitions.front().authority.authority_class ==
              fpm::PartitionAuthorityClass::Primary,
          "the initial authority is primary");

  fpm::IsolationRequest isolate_request;
  isolate_request.attempt = fpm_example::attempt_token(1);
  isolate_request.expected_epoch = runtime.epoch();
  isolate_request.cause = fpm::ReasonCode::IsolationApplied;
  isolate_request.requester = fpm::Provenance::from_validated("example");
  isolate_request.lineage = lineage;
  isolate_request.duration_ticks = 0;
  const fpm::IsolationOutcome isolated = runtime.isolate(isolate_request);
  require(isolated.verdict == fpm::DecisionVerdict::Isolated,
          "applying an isolation directive must be reported as ISOLATED: " +
              isolated.decision.render());
  require(runtime.status().active_isolations == 1, "the isolation directive must be active");

  const fpm::PartitionAssessment degraded = runtime.last_assessment();
  const fpm::Partition* const isolated_partition =
      fpm_example::find_by_lineage(degraded.partitions, lineage);
  if (isolated_partition == nullptr) {
    fpm_example::fail("the isolated lineage must still be present");
  }
  require(isolated_partition->authority.authority_class == fpm::PartitionAuthorityClass::Isolated,
          "the isolated lineage must report ISOLATED");
  require(isolated_partition->authority.basis == fpm::AuthorityBasis::ExplicitIsolation,
          "the basis must name the explicit isolation");
  require(isolated_partition->authority.authorized.empty(),
          "an isolated subject holds no capability at all");
  require(isolated_partition->authority.denied.count() == fpm::authority_capability_count,
          "everything physically possible is denied while isolated");
  require(!isolated_partition->authoritative, "an isolated subject is not authoritative");
  require(degraded.decision.verdict == fpm::DecisionVerdict::Isolated,
          "the assessment of an isolated fabric is ISOLATED");
  std::cout << "isolated lineage=" << lineage.view()
            << " class=" << fpm::partition_authority_class_name(
                                   isolated_partition->authority.authority_class)
            << " basis=" << fpm::authority_basis_name(isolated_partition->authority.basis)
            << " authorized=[" << isolated_partition->authority.authorized.render() << "]"
            << " verdict=" << fpm::decision_verdict_name(degraded.decision.verdict) << "\n";

  fpm::IsolationRequest clear_request;
  clear_request.attempt = fpm_example::attempt_token(2);
  clear_request.expected_epoch = runtime.epoch();
  clear_request.cause = fpm::ReasonCode::IsolationApplied;
  clear_request.requester = fpm::Provenance::from_validated("example");
  clear_request.lineage = lineage;
  const fpm::IsolationOutcome cleared = runtime.clear_isolation(clear_request);
  require(cleared.verdict == fpm::DecisionVerdict::Granted,
          "clearing an isolation directive must be granted: " + cleared.decision.render());
  require(runtime.status().active_isolations == 0, "no isolation directive remains active");

  const fpm::PartitionAssessment recovered = runtime.last_assessment();
  const fpm::Partition* const recovered_partition =
      fpm_example::find_by_lineage(recovered.partitions, lineage);
  if (recovered_partition == nullptr) {
    fpm_example::fail("the recovered lineage must be present");
  }
  require(recovered_partition->authority.authority_class == fpm::PartitionAuthorityClass::Primary,
          "clearing the directive must restore primary authority");
  require(recovered_partition->authority.authorized.count() == fpm::authority_capability_count,
          "recovery restores every capability");
  require(recovered.decision.verdict == fpm::DecisionVerdict::Granted,
          "the assessment after recovery is a grant");
  std::cout << "recovered lineage=" << lineage.view()
            << " class=" << fpm::partition_authority_class_name(
                                   recovered_partition->authority.authority_class)
            << " authorized=[" << recovered_partition->authority.authorized.render() << "]"
            << " verdict=" << fpm::decision_verdict_name(recovered.decision.verdict) << "\n";
}

}  // namespace

int main() { return fpm_example::run_example("ex_isolation_and_degradation", &body); }
