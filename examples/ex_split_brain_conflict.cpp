// Fabric Partition Manager 1.0.0 - Summon Software Labs
//
// ex_split_brain_conflict: two partitions that each satisfy the full authority
// quorum are a CONFLICT, and both are reduced to observe-only.
//
// The policy is a plain component-count quorum, so each half of a split fabric
// qualifies on its own. Two independent primaries are never resolved by
// preferring one of them: the runtime refuses to pick a winner and reports the
// conflict, leaving both component sets with observation alone.
#include <cstddef>
#include <iostream>
#include <vector>

#include "example_support.hpp"

namespace {

namespace fpm = fabric_partition_manager;
using fpm_example::require;

void body() {
  const fpm::PartitionPolicy policy = fpm_example::count_quorum_policy(1, 3);
  fpm::PartitionRuntime runtime(fpm_example::memory_options(policy));
  (void)fpm_example::setup_synthetic(runtime, {3, 3});

  const fpm::PartitionAssessment assessment = runtime.assess();
  require(assessment.confirmed, "the split must be confirmed by complete coverage");
  require(assessment.partitions.size() == 2, "the fabric must decompose into two partitions");
  require(assessment.split_brain, "two qualifying partitions must be reported as split brain");
  require(assessment.decision.verdict == fpm::DecisionVerdict::Conflict,
          "a split brain is a CONFLICT verdict");
  require(assessment.authoritative_partition.is_nil(),
          "a conflict must not name an authoritative partition");
  require(assessment.fabric_authority != fpm::PartitionAuthorityClass::Primary,
          "a conflict must not report a primary authority");

  for (const fpm::Partition& partition : assessment.partitions) {
    require(partition.authority.authority_class == fpm::PartitionAuthorityClass::ObserveOnly,
            "each conflicting partition must be reduced to observe-only");
    require(partition.authority.basis == fpm::AuthorityBasis::SplitBrainConflict,
            "the basis must name the split-brain conflict");
    require(partition.authority.authorized.count() == 1 &&
                partition.authority.authorized.contains(fpm::AuthorityCapability::Observe),
            "observe-only carries exactly the observe capability");
    require(!partition.authority.authorized.contains(fpm::AuthorityCapability::ServeWrites),
            "a conflicting partition may not write");
    require(!partition.authority.authorized.contains(fpm::AuthorityCapability::IssueLeases),
            "a conflicting partition may not issue leases");
    require(!partition.authoritative, "a conflicting partition is not authoritative");
  }

  std::cout << "split-brain=true verdict="
            << fpm::decision_verdict_name(assessment.decision.verdict)
            << " partitions=" << assessment.partitions.size()
            << " authoritative-partition=none" << "\n";
  for (const fpm::Partition& partition : assessment.partitions) {
    std::cout << "partition members=" << partition.member_count()
              << " class=" << fpm::partition_authority_class_name(
                                     partition.authority.authority_class)
              << " basis=" << fpm::authority_basis_name(partition.authority.basis)
              << " authorized=[" << partition.authority.authorized.render() << "]" << "\n";
  }
}

}  // namespace

int main() { return fpm_example::run_example("ex_split_brain_conflict", &body); }
